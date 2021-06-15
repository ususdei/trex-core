#include "gtp_man.h"
#include "common/Network/Packet/EthernetHeader.h"
#include "common/Network/Packet/GTPUHeader.h"
#include "common/Network/Packet/VLANHeader.h"
#include "common/Network/Packet/TcpHeader.h"
#include "common/Network/Packet/UdpHeader.h"
#include "rpc-server/trex_rpc_cmd_api.h"
#include "stx/common/trex_rx_rpc_tunnel.h"
#include "sys/socket.h"
#include "arpa/inet.h"
#include "bp_sim.h"
#define ENCAPSULATION_LEN 36
#define ENCAPSULATION6_LEN 56
#define DEST_PORT 2152
#define IPV4_HDR_TTL 64
#define HOP_LIMIT 255


int CGtpuMan::on_tx(uint8_t dir, rte_mbuf *pkt) {
    assert((m_mode & TUNNEL_MODE_TX) == TUNNEL_MODE_TX);
    if (dir != m_port_id) {
        return -1;
    }
    tunnel_cntxt_t tunnel_context = pkt->dynfield_ptr;
    if (!tunnel_context){
        return -1;
    }
    prepend(pkt);
    return 0;
}


int CGtpuMan::on_rx(uint8_t dir, rte_mbuf *pkt) {
    assert((m_mode & TUNNEL_MODE_RX) == TUNNEL_MODE_RX);
    if (dir != m_port_id) {
        return -1;
    }
    adjust(pkt);
    return 0;
}


void* CGtpuMan::get_tunnel_context(client_tunnel_data_t *data) {
    assert((m_mode & TUNNEL_MODE_DP) == TUNNEL_MODE_DP);
    void* gtpu = NULL;
    if (data->version == 4) {
        gtpu = (void*) new CGtpuCtx(data->teid,
                                    data->src.ipv4,
                                    data->dst.ipv4);
    } else {
        gtpu = (void*) new CGtpuCtx(data->teid,
                                    &(data->dst.ipv6),
                                    &(data->dst.ipv6));
    }
    return gtpu;
}


void CGtpuMan::delete_tunnel_context(void *tunnel_context){
    if (tunnel_context){
        delete((CGtpuCtx *)tunnel_context);
    }
}


tunnel_type CGtpuMan::get_tunnel_type() {
    return TUNNEL_TYPE_GTP;
}


string CGtpuMan::get_tunnel_type_str() {
    return "GTP";
}


void CGtpuMan::update_tunnel_context(client_tunnel_data_t *data, void *tunnel_context) {
    assert((m_mode & TUNNEL_MODE_DP) == TUNNEL_MODE_DP);
    CGtpuCtx* gtpu = (CGtpuCtx *)tunnel_context;
    if (data->version == 4) {
        gtpu->update_ipv4_gtpu_info(data->teid,
                                    data->src.ipv4,
                                    data->dst.ipv4);
    } else {
        gtpu->update_ipv6_gtpu_info(data->teid,
                                    &(data->dst.ipv6),
                                    &(data->dst.ipv6));
    }
}


void CGtpuMan::parse_tunnel(const Json::Value &params, Json::Value &result, std::vector<client_tunnel_data_t> &all_msg_data) {
    assert((m_mode & TUNNEL_MODE_CP) == TUNNEL_MODE_CP);
    CRpcTunnelBatch parser;
    const Json::Value &attr = parser.parse_array(params, "attr", result);
   
    for (auto each_client : attr) {
        client_tunnel_data_t msg_data;
        string src_ipv46, dst_ipv46;

        msg_data.version     = parser.parse_uint32(each_client, "version", result);
        msg_data.client_ip   = parser.parse_uint32(each_client, "client_ip", result);
        src_ipv46            = parser.parse_string(each_client, "sip", result);
        dst_ipv46            = parser.parse_string(each_client, "dip", result);
        if (msg_data.version == 6) {
           inet_pton(AF_INET6, src_ipv46.c_str(), msg_data.src.ipv6.addr);
           inet_pton(AF_INET6, dst_ipv46.c_str(), msg_data.dst.ipv6.addr);
        } else {
           inet_pton(AF_INET, src_ipv46.c_str(), &msg_data.src.ipv4);
           inet_pton(AF_INET, dst_ipv46.c_str(), &msg_data.dst.ipv4);
        }
        msg_data.teid = parser.parse_uint32(each_client, "teid", result);
        all_msg_data.push_back(msg_data);
    }
}


void CGtpuMan::parser_options() {
    assert(((m_mode & TUNNEL_MODE_RX) == TUNNEL_MODE_RX) && ((m_mode & TUNNEL_MODE_TX) == TUNNEL_MODE_TX));
    CParserOption *po = &CGlobalInfo::m_options;
    po->preview.setTsoOffloadDisable(true);
    po->preview.setLroOffloadDisable(true);
    CDpdkMode *mode = &CGlobalInfo::m_dpdk_mode;
    mode->force_software_mode(true);
}



/*
* Add encapsulation ([ipv4/ipv6]/udp/gtpu) to the packet with the ip.src/dst and teid that we set in the
* constructor.
* In order for the Prepend method to work, the headroom in the rte_mbuf has to be large enough for the
* encapsulation we used (36 bytes for ipv4 and 56 bytes for ipv6).
* The Prepend method works only with [ipv4/ipv6]/[tcp/udp] packets.
* The IP header type in the encapsulation is determined by the constructor we used.
* In the udp header the checksum is being calculated by relying on the inner [udp/tcp] checksum.
* udp.checksum = ~inner.ips.checksum + outer.ips.checksum + checksum(udp_start - inner[tcp/udp]offset).
* For example:
* input: [ipv4/ipv6]-inner/[udp/tcp]-inner/py. and we chose the ipv4 constructor.
* output: ipv4-encp/udp-encp/gtpu-encp/[ipv4/ipv6]-inner/[udp/tcp]-inner/py
* return valuse: 0 in case of success otherwise -1.
*/
int CGtpuMan::prepend(rte_mbuf *pkt) {
    if (pkt == NULL) {
        return -1;
    }
    if (pkt->buf_addr == NULL || pkt->buf_len == 0) {
        return -1;
    }
    CGtpuCtx *gtp_context = (CGtpuCtx *)pkt->dynfield_ptr;
    EthernetHeader *eth = rte_pktmbuf_mtod(pkt, EthernetHeader *);
    VLANHeader *vh;
    uint16_t l3_offset = 0;
    uint16_t l2_nxt_hdr = eth->getNextProtocol();
    l3_offset += ETH_HDR_LEN;
    if (l2_nxt_hdr  == EthernetHeader::Protocol::VLAN) {
        vh = (VLANHeader *)((uint8_t *)eth + ETH_HDR_LEN);
        l2_nxt_hdr = vh->getNextProtocolHostOrder();
        l3_offset += VLAN_HDR_LEN;
    }
    if (l2_nxt_hdr != EthernetHeader::Protocol::IP && l2_nxt_hdr != EthernetHeader::Protocol::IPv6) {
        return -1;
    }
    IPHeader *iph = rte_pktmbuf_mtod_offset (pkt, IPHeader *, l3_offset);
    uint8_t l4_offset = l3_offset + IPV4_HDR_LEN;
    uint16_t inner_cs = 0;
    uint8_t l3_nxt_hdr;
    if (iph->getVersion() == IPHeader::Protocol::IP) {
        IPPseudoHeader ips((IPHeader *)iph);
        inner_cs = ~ips.inetChecksum();
        l3_nxt_hdr = ((IPHeader *)iph)->getNextProtocol();
    } else {
        l4_offset = l3_offset + IPV6_HDR_LEN;
        IPv6PseudoHeader ips((IPv6Header *)iph);
        inner_cs = ~ips.inetChecksum();
        l3_nxt_hdr = ((IPv6Header *)iph)->getNextHdr();
    }
    if (l3_nxt_hdr != IPHeader::Protocol::UDP && l3_nxt_hdr != IPHeader::Protocol::TCP){
        return -1;
    }
    int res = 0;
    if (gtp_context->is_ipv6() == 0) {
        res = prepend_ipv4_tunnel(pkt, l4_offset, inner_cs, gtp_context);
    } else {
        res = prepend_ipv6_tunnel(pkt, l4_offset, inner_cs, gtp_context);
    }
    return res;
}


/*
* Removes the encapsulation headers [ipv4/ipv6]/udp/gtpu from the packet,
* In case there is indeed encapsulation with those headers.
* For example :
* input: [ipv4/ipv6]-encp/udp-encp/gtpu-encp/[ipv4/ipv6]-inner/[udp/tcp]-inner/py
* output: [ipv4/ipv6]-inner/[udp/tcp]-inner/py
* return valuse: 0 in case of success otherwise -1.
*/
int CGtpuMan::adjust(rte_mbuf *pkt) {
    if (pkt == NULL) {
        return -1;
    }
    if (pkt->buf_addr == NULL || pkt->data_len < 36) {
        return -1;
    }
    EthernetHeader *eth = rte_pktmbuf_mtod(pkt, EthernetHeader *);
    if (eth->getNextProtocol() == EthernetHeader::Protocol::ARP) {
        return -1;
    }
    uint8_t l3_offset = ETH_HDR_LEN;
    uint16_t l2_nxt_hdr = eth->getNextProtocol();
    IPHeader *iph = (IPHeader *)((uint8_t *)eth + ETH_HDR_LEN);
    if (l2_nxt_hdr == EthernetHeader::Protocol::VLAN) {
        VLANHeader *vh;
        vh = (VLANHeader *)((uint8_t *)eth + ETH_HDR_LEN);
        l2_nxt_hdr = vh->getNextProtocolHostOrder();
        iph = (IPHeader *)((uint8_t *)vh + VLAN_HDR_LEN);
        l3_offset += VLAN_HDR_LEN;
    }
    if (l2_nxt_hdr != EthernetHeader::Protocol::IP && l2_nxt_hdr != EthernetHeader::Protocol::IPv6) {
        return -1;
    }
    int res = 0;
    if (iph->getVersion() == IPHeader::Protocol::IP) {
        res = adjust_ipv4_tunnel(pkt, eth, l3_offset);
    } else {
        res = adjust_ipv6_tunnel(pkt,eth, l3_offset);
    }
    if(res != 0) {
        return -1;
    }
    uint16_t l2_len = 0;
    EthernetHeader *old_eth = (EthernetHeader *)eth;
    EthernetHeader *new_eth = rte_pktmbuf_mtod(pkt, EthernetHeader *);
    new_eth->mySource = old_eth->mySource;
    new_eth->myDestination = old_eth->myDestination;
    l2_len = ETH_HDR_LEN;
    if (old_eth->getNextProtocol() == EthernetHeader::Protocol::VLAN) {
        VLANHeader *vlan_hdr = (VLANHeader *)((uint8_t *)new_eth + ETH_HDR_LEN);
        VLANHeader *vh = (VLANHeader *)((uint8_t *)old_eth + ETH_HDR_LEN);
        new_eth->setNextProtocol(old_eth->getNextProtocol());
        l2_len += VLAN_HDR_LEN;
        iph = rte_pktmbuf_mtod_offset(pkt, IPHeader *, l2_len);
        vlan_hdr->myTag = vh->myTag;
        if (iph->getVersion() == IPHeader::Protocol::IP) {
            vlan_hdr->setNextProtocolFromHostOrder(EthernetHeader::Protocol::IP);
        } else {
            vlan_hdr->setNextProtocolFromHostOrder(EthernetHeader::Protocol::IPv6);
        }
    } else {
        iph = rte_pktmbuf_mtod_offset(pkt, IPHeader *, l2_len);
        if (iph->getVersion() == IPHeader::Protocol::IP) {
            new_eth->setNextProtocol(EthernetHeader::Protocol::IP);
        } else {
            new_eth->setNextProtocol(EthernetHeader::Protocol::IPv6);
        }
    }
    return 0;
}


int CGtpuMan::prepend_ipv4_tunnel(rte_mbuf * pkt, uint8_t l4_offset, uint16_t inner_cs, CGtpuCtx *gtp_context) {
    if (rte_pktmbuf_headroom(pkt) < ENCAPSULATION_LEN) {
        return -1;
    }
    EthernetHeader *eth = rte_pktmbuf_mtod(pkt, EthernetHeader *);
    TCPHeader *tch = rte_pktmbuf_mtod_offset (pkt, TCPHeader *, l4_offset);
    VLANHeader *vh;
    uint8_t *encap = (uint8_t *) rte_pktmbuf_prepend(pkt, ENCAPSULATION_LEN);
    memset(encap, 0, ENCAPSULATION_LEN);
    EthernetHeader *outer_eth = (EthernetHeader *)encap;
    outer_eth->mySource=eth->mySource;
    outer_eth->myDestination=eth->myDestination;
    IPHeader *outer_ipv4 = (IPHeader *)((uint8_t *)outer_eth + ETH_HDR_LEN);
    if (eth->getNextProtocol() == EthernetHeader::Protocol::VLAN) {
        outer_eth->setNextProtocol(EthernetHeader::Protocol::VLAN);
        VLANHeader *vlan_hdr = (VLANHeader *)((uint8_t *)outer_eth + ETH_HDR_LEN); 
        vh = (VLANHeader *)((uint8_t *)eth + ETH_HDR_LEN);
        vlan_hdr->setNextProtocolFromHostOrder(EthernetHeader::Protocol::IP);
        vlan_hdr->myTag = vh->myTag;
        outer_ipv4 = (IPHeader *)((uint8_t *)vlan_hdr + VLAN_HDR_LEN);
        /*Copy the pre-cooked packet*/
        memcpy((void*)outer_ipv4, (void *)gtp_context->get_outer_hdr(), ENCAPSULATION_LEN);
        /*Fix outer header IPv4 length */
        outer_ipv4->setTotalLength((uint16_t)(pkt->pkt_len - (ETH_HDR_LEN  + VLAN_HDR_LEN)));
    } else {
        outer_eth->setNextProtocol(EthernetHeader::Protocol::IP);
        /*Copy the pre-cooked packet at the right place*/
        memcpy((void*)outer_ipv4, (void *)gtp_context->get_outer_hdr(), ENCAPSULATION_LEN);
        /*Fix outer header IPv4 length */
        outer_ipv4->setTotalLength((uint16_t)(pkt->pkt_len - ETH_HDR_LEN));
    }
    /*Fix ipv4 checksum*/
    outer_ipv4->updateCheckSumFast();
    /*Fix UDP header length */
    UDPHeader* udp = (UDPHeader *)((uint8_t *)outer_ipv4 + IPV4_HDR_LEN);
    udp->setSourcePort(tch->getSourcePort());
    udp->setLength((uint16_t)(outer_ipv4->getTotalLength() - IPV4_HDR_LEN));
    /*Fix GTPU length*/
    GTPUHeader *gtpu = (GTPUHeader *)(udp+1);
    gtpu->setLength(udp->getLength() - UDP_HEADER_LEN - GTPU_V1_HDR_LEN);
    udp->setChecksumRaw(udp->calcCheckSum(outer_ipv4, (uint8_t *)tch - (uint8_t*)udp, inner_cs));
    return 0;
}


int CGtpuMan::prepend_ipv6_tunnel(rte_mbuf * pkt, uint8_t l4_offset, uint16_t inner_cs, CGtpuCtx *gtp_context) {
    if (rte_pktmbuf_headroom(pkt) < ENCAPSULATION6_LEN) {
        return -1;
    }
    EthernetHeader * eth = rte_pktmbuf_mtod(pkt, EthernetHeader *);
    TCPHeader *tch = rte_pktmbuf_mtod_offset (pkt, TCPHeader *, l4_offset);
    VLANHeader *vh; 
    uint8_t * encap6 = (uint8_t *) rte_pktmbuf_prepend(pkt, ENCAPSULATION6_LEN);
    memset(encap6, 0, ENCAPSULATION6_LEN);
    EthernetHeader * outer_eth = (EthernetHeader *)encap6;
    outer_eth->mySource=eth->mySource;
    outer_eth->myDestination=eth->myDestination;
    IPv6Header * outer_ipv6 = (IPv6Header *)((uint8_t *)outer_eth + ETH_HDR_LEN);
    if (eth->getNextProtocol() == EthernetHeader::Protocol::VLAN) {
        outer_eth->setNextProtocol(EthernetHeader::Protocol::VLAN);
        VLANHeader *vlan_hdr = (VLANHeader *)((uint8_t *)outer_eth+ETH_HDR_LEN); 
        vh = (VLANHeader *)((uint8_t *)eth + ETH_HDR_LEN);
        vlan_hdr->setNextProtocolFromHostOrder(EthernetHeader::Protocol::IPv6);
        vlan_hdr->myTag = vh->myTag;
        /*outerether type to be Ipv6 , as its IPv6 tunnel */
        outer_ipv6 = (IPv6Header *)((uint8_t *)vlan_hdr + VLAN_HDR_LEN);
        /*Copy the pre-cooked packet*/
        memcpy((void*)outer_ipv6, (void *)gtp_context->get_outer_hdr(), ENCAPSULATION6_LEN);
        /*Fix outer header IPv6 length */
        outer_ipv6->setPayloadLen(pkt->pkt_len - (ETH_HDR_LEN  + VLAN_HDR_LEN + IPV6_HDR_LEN));
    } else {
        /*Override outerether type to be Ipv6 , as its IPv6 tunnel */
        outer_eth->setNextProtocol(EthernetHeader::Protocol::IPv6);
        /*Copy the pre-cooked packet at the right place*/
        memcpy((void*)outer_ipv6, (void *)gtp_context->get_outer_hdr(), ENCAPSULATION6_LEN);
        /*Fix outer header IPv6 length */
        outer_ipv6->setPayloadLen(pkt->pkt_len - (ETH_HDR_LEN + IPV6_HDR_LEN));
    }
    /*Fix UDP length*/
    UDPHeader* udp = (UDPHeader *)((uint8_t *)outer_ipv6 + IPV6_HDR_LEN);
    udp->setSourcePort(tch->getSourcePort());
    udp->setLength((uint16_t)(outer_ipv6->getPayloadLen()));
    /*Fix GTPU length*/
    GTPUHeader *gtpu = (GTPUHeader *)(udp+1);
    gtpu->setLength(udp->getLength() - UDP_HEADER_LEN - GTPU_V1_HDR_LEN);
    udp->setChecksumRaw(udp->calcCheckSum(outer_ipv6, (uint8_t *)tch - (uint8_t*)udp, inner_cs));
    return 0;
}


int CGtpuMan::adjust_ipv6_tunnel(rte_mbuf * pkt, void *eth,  uint8_t l3_offset) {
    if(pkt->data_len < ENCAPSULATION6_LEN) {
        return -1;
    }
    IPv6Header *ipv6 = rte_pktmbuf_mtod_offset (pkt, IPv6Header *, l3_offset);
    if (ipv6->getNextHdr() != IPHeader::Protocol::UDP) {
        return -1;
    }
    UDPHeader *udp ;
    udp = (UDPHeader *) ((uint8_t *)ipv6 + IPV6_HDR_LEN);
    if (validate_gtpu_udp((void *)udp) == -1){
        return -1;
    }
    rte_pktmbuf_adj(pkt, ENCAPSULATION6_LEN);
    return 0;
}


int CGtpuMan::adjust_ipv4_tunnel(rte_mbuf * pkt, void *eth, uint8_t l3_offset) {
    IPHeader *iph =  rte_pktmbuf_mtod_offset (pkt, IPHeader *,l3_offset);
    if (iph->getProtocol() != IPHeader::Protocol::UDP){
        return -1;
    }
    UDPHeader* udp ;
    udp = (UDPHeader*) ((uint8_t *)iph + IPV4_HDR_LEN);
    if (validate_gtpu_udp((void *)udp) == -1){
        return -1;
    }
    rte_pktmbuf_adj(pkt, ENCAPSULATION_LEN);
    return 0;
}


int CGtpuMan::validate_gtpu_udp(void *udp) {
    if (((UDPHeader *)udp)->getDestPort() != DEST_PORT) {
        return -1;
    }
    GTPUHeader *gtpu = (GTPUHeader*)((uint8_t *)udp + UDP_HEADER_LEN);
    if (gtpu->getType() != GTPU_TYPE_GTPU){
        return -1;
    }
    return 0;
}

/************** CGCGtpuCtx class ******************/

int32_t CGtpuCtx::get_teid() {
    assert(m_outer_hdr != NULL);
    return m_teid;
}


ipv4_addr_t CGtpuCtx::get_src_ipv4() {
    assert(!m_ipv6_set);
    return m_src.ipv4;
}


ipv4_addr_t CGtpuCtx::get_dst_ipv4() {
    assert(!m_ipv6_set);
    return m_dst.ipv4;
}


void CGtpuCtx::get_src_ipv6(ipv6_addr_t* src) {
    assert(m_ipv6_set);
    *src = m_src.ipv6;
}


void CGtpuCtx::get_dst_ipv6(ipv6_addr_t* dst) {
    assert(m_ipv6_set);
    *dst = m_dst.ipv6;
}


void * CGtpuCtx::get_outer_hdr() {
    return (void*)m_outer_hdr;
}


bool CGtpuCtx::is_ipv6() {
    return m_ipv6_set;
}


CGtpuCtx::CGtpuCtx(uint32_t teid, ipv4_addr_t src_ip, ipv4_addr_t dst_ip) {
    m_outer_hdr = (uint8_t *)malloc(ENCAPSULATION_LEN);
    store_ipv4_members(teid, src_ip, dst_ip);
    store_ipv4_gtpu_info();
}


CGtpuCtx::CGtpuCtx(uint32_t teid, ipv6_addr_t* src_ipv6, ipv6_addr_t* dst_ipv6) {
    m_outer_hdr = (uint8_t *)malloc(ENCAPSULATION6_LEN);
    store_ipv6_members(teid, src_ipv6, dst_ipv6);
    store_ipv6_gtpu_info();
}


CGtpuCtx::~CGtpuCtx() {
    free(m_outer_hdr);
}


void CGtpuCtx::update_ipv4_gtpu_info(uint32_t teid, ipv4_addr_t src_ip, ipv4_addr_t dst_ip) {
    store_ipv4_members(teid, src_ip, dst_ip);
    store_ipv4_gtpu_info();
}


void CGtpuCtx::update_ipv6_gtpu_info(uint32_t teid, ipv6_addr_t* src_ipv6, ipv6_addr_t* dst_ipv6) {
    store_ipv6_members(teid, src_ipv6, dst_ipv6);
    store_ipv6_gtpu_info();
}


void CGtpuCtx::store_ipv4_members(uint32_t teid, ipv4_addr_t src_ip, ipv4_addr_t dst_ip) {
    m_teid = teid;
    m_src.ipv4 = src_ip;
    m_dst.ipv4 = dst_ip;
    m_ipv6_set = false;
}


void CGtpuCtx::store_ipv6_members(uint32_t teid, ipv6_addr_t* src_ipv6, ipv6_addr_t* dst_ipv6) {
    m_teid = teid;
    m_src.ipv6 = *src_ipv6;
    m_dst.ipv6 = *dst_ipv6;
    m_ipv6_set = true;
}


void CGtpuCtx::store_ipv4_gtpu_info() {
    memset(m_outer_hdr, 0, ENCAPSULATION_LEN);
    IPHeader *ip = (IPHeader *)(m_outer_hdr);
    ip->setVersion(IPHeader::Protocol::IP);
    ip->setHeaderLength(IPV4_HDR_LEN);
    ip->setTimeToLive(IPV4_HDR_TTL);
    ip->setProtocol(IPHeader::Protocol::UDP);
    ip->mySource = m_src.ipv4;
    ip->myDestination = m_dst.ipv4;
    UDPHeader *udp = (UDPHeader *)(m_outer_hdr + IPV4_HDR_LEN);
    store_udp_gtpu((void *)udp);
}


void CGtpuCtx::store_ipv6_gtpu_info() {
    memset(m_outer_hdr, 0, ENCAPSULATION6_LEN);
    IPv6Header *ip6 = (IPv6Header *)(m_outer_hdr);
    ip6->setVersion(6);
    ip6->setHopLimit(HOP_LIMIT);
    ip6->setNextHdr(IPHeader::Protocol::UDP);
    memcpy((void*)ip6->mySource ,(void *)m_src.ipv6.addr, IPV6_ADDR_LEN);
    memcpy((void*)ip6->myDestination, (void *)m_dst.ipv6.addr ,IPV6_ADDR_LEN);
    UDPHeader *udp = (UDPHeader *)(m_outer_hdr + IPV6_HDR_LEN);
    store_udp_gtpu((void *)udp);
}


void CGtpuCtx::store_udp_gtpu(void *udp){
    ((UDPHeader *)udp)->setDestPort((uint16_t)(DEST_PORT));
    ((UDPHeader *)udp)->setChecksum(0);
    GTPUHeader *gtpu = (GTPUHeader *)((uint8_t *)udp + UDP_HEADER_LEN);
    memset(gtpu, 0, GTPU_V1_HDR_LEN);
    gtpu->setVerFlags(GTPU_V1_VER | GTPU_PT_BIT);
    gtpu->setType(GTPU_TYPE_GTPU);
    gtpu->setTeid(m_teid);
}
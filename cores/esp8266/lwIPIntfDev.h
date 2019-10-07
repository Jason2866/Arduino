
#ifndef _LWIPINTFDEV_H
#define _LWIPINTFDEV_H

// TODO:
// remove all Serial.print
// unchain pbufs

#include <netif/ethernet.h>
#include <lwip/init.h>
#include <lwip/netif.h>
#include <lwip/etharp.h>
#include <lwip/dhcp.h>

#include <user_interface.h>	// wifi_get_macaddr()

#include "Schedule.h"
#include "lwIPIntf.h"
#include "wl_definitions.h"

#ifndef DEFAULT_MTU
#define DEFAULT_MTU 1500
#endif

template <class RawDev>
class LwipIntfDev: public LwipIntf, public RawDev {

public:

    LwipIntfDev (int8_t cs = SS, SPIClass& spi = SPI, int8_t intr = -1):
        RawDev(cs, spi, intr),
        _mtu(DEFAULT_MTU),
        _default(false),
        _intrPin(intr)
    {
        memset(&_netif, 0, sizeof(_netif));
    }
    
    boolean config (const IPAddress& local_ip, const IPAddress& arg1, const IPAddress& arg2, const IPAddress& arg3, const IPAddress& dns2);

    // start with dhcp client
    // default mac-address is inferred from esp8266's STA interface
    boolean begin (const uint8_t *macAddress = nullptr, uint16_t mtu = DEFAULT_MTU);

    const netif* getNetIf   () const { return &_netif; }

#if LWIP_VERSION_MAJOR == 1
    IPAddress    localIP    () const { return IPAddress(_netif.ip_addr.u_addr.ip4.addr); }
    IPAddress    subnetMask () const { return IPAddress(_netif.netmask.u_addr.ip4.addr); }
    IPAddress    gatewayIP  () const { return IPAddress(_netif.gw.u_addr.ip4.addr); }
#else
    IPAddress    localIP    () const { return IPAddress(ip4_addr_get_u32(ip_2_ip4(&_netif.ip_addr))); }
    IPAddress    subnetMask () const { return IPAddress(ip4_addr_get_u32(ip_2_ip4(&_netif.netmask))); }
    IPAddress    gatewayIP  () const { return IPAddress(ip4_addr_get_u32(ip_2_ip4(&_netif.gw))); }
#endif

    void setDefault ();

    bool connected () { return !!ip4_addr_get_u32(ip_2_ip4(&_netif.ip_addr)); }

    // ESP8266WiFi API compatibility

    wl_status_t status ();

protected:

    netif _netif;
    uint16_t _mtu;
    bool _default;
    int8_t _intrPin;
    uint8_t _macAddress[6];
    bool _started = false;

    err_t start_with_dhclient ();
    err_t netif_init ();
    void  netif_status_callback ();

    static err_t netif_init_s (netif* netif);
    static err_t linkoutput_s (netif *netif, struct pbuf *p);
    static void  netif_status_callback_s (netif* netif);

    // called on a regular basis or on interrupt
    err_t handlePackets ();
};

template <class RawDev>
boolean LwipIntfDev<RawDev>::config (const IPAddress& localIP, const IPAddress& gateway, const IPAddress& netmask, const IPAddress& dns1, const IPAddress& dns2)
{
    if (_started)
    {
        DEBUGV("LwipIntfDev: use config() then begin()\n");
        return false;
    }
    
    IPAddress realGateway, realNetmask, realDns1;
    if (!ipAddressReorder(localIP, gateway, netmask, dns1, realGateway, realGateway, realDns1))
        return false;
    ip4_addr_set_u32(ip_2_ip4(&_netif.ip_addr), localIP.v4());
    ip4_addr_set_u32(ip_2_ip4(&_netif.gw), realGateway.v4());
    ip4_addr_set_u32(ip_2_ip4(&_netif.netmask), realNetmask.v4());
    
    return true;
}

template <class RawDev>
boolean LwipIntfDev<RawDev>::begin (const uint8_t* macAddress, uint16_t mtu)
{
    if (macAddress)
        memcpy(_macAddress, macAddress, 6);
    else
    {
        _netif.num = 2;
        for (auto n = netif_list; n; n = n->next)
            if (n->num >= _netif.num)
                _netif.num = n->num + 1;

#if 1
        // forge a new mac-address from the esp's wifi sta one
        // I understand this is cheating with an official mac-address
        wifi_get_macaddr(STATION_IF, (uint8*)_macAddress);
#else
        // https://serverfault.com/questions/40712/what-range-of-mac-addresses-can-i-safely-use-for-my-virtual-machines
        memset(_macAddress, 0, 6);
        _macAddress[0] = 0xEE;
#endif
        _macAddress[3] += _netif.num;
        memcpy(_netif.hwaddr, _macAddress, 6);
    }

    if (!RawDev::begin(_macAddress))
        return false;
    _started = true;
    _mtu = mtu;

    if (localIP().v4() == 0)
        switch (start_with_dhclient())
        {
        case ERR_OK:
            break;

        case ERR_IF:
            return false;

        default:
            netif_remove(&_netif);
            return false;
        }

    if (_intrPin >= 0)
    {
        if (RawDev::interruptIsPossible())
        {
            //attachInterrupt(_intrPin, [&]() { this->handlePackets(); }, FALLING);
        }
        else
        {
            ::printf((PGM_P)F("lwIP_Intf: Interrupt not implemented yet, enabling transparent polling\r\n"));
            _intrPin = -1;
        }
    }

    if (_intrPin < 0 && !schedule_recurrent_function_us([&]() { this->handlePackets(); return true; }, 100))
    {
        netif_remove(&_netif);
        return false;
    }

    return true;
}

template <class RawDev>
wl_status_t LwipIntfDev<RawDev>::status ()
{
    return _started? (connected()? WL_CONNECTED: WL_DISCONNECTED): WL_NO_SHIELD;
}

template <class RawDev>
err_t LwipIntfDev<RawDev>::start_with_dhclient ()
{
    ip4_addr_t ip, mask, gw;

    ip4_addr_set_zero(&ip);
    ip4_addr_set_zero(&mask);
    ip4_addr_set_zero(&gw);

    _netif.hwaddr_len = sizeof _macAddress;
    memcpy(_netif.hwaddr, _macAddress, sizeof _macAddress);

    if (!netif_add(&_netif, &ip, &mask, &gw, this, netif_init_s, ethernet_input))
        return ERR_IF;

    _netif.flags |= NETIF_FLAG_UP;

    return dhcp_start(&_netif);
}

template <class RawDev>
err_t LwipIntfDev<RawDev>::linkoutput_s (netif *netif, struct pbuf *pbuf)
{
    LwipIntfDev* ths = (LwipIntfDev*)netif->state;

    if (pbuf->len != pbuf->tot_len || pbuf->next)
        Serial.println("ERRTOT\r\n");

    uint16_t len = ths->sendFrame((const uint8_t*)pbuf->payload, pbuf->len);

#if PHY_HAS_CAPTURE
    if (phy_capture)
        phy_capture(ths->_netif.num, (const char*)pbuf->payload, pbuf->len, /*out*/1, /*success*/len == pbuf->len);
#endif

    return len == pbuf->len? ERR_OK: ERR_MEM;
}

template <class RawDev>
err_t LwipIntfDev<RawDev>::netif_init_s (struct netif* netif)
{
    return ((LwipIntfDev*)netif->state)->netif_init();
}

template <class RawDev>
void LwipIntfDev<RawDev>::netif_status_callback_s (struct netif* netif)
{
    ((LwipIntfDev*)netif->state)->netif_status_callback();
}

template <class RawDev>
err_t LwipIntfDev<RawDev>::netif_init ()
{
    _netif.name[0] = 'e';
    _netif.name[1] = '0' + _netif.num;
    _netif.mtu = _mtu;
    _netif.chksum_flags = NETIF_CHECKSUM_ENABLE_ALL;
    _netif.flags =
          NETIF_FLAG_ETHARP
        | NETIF_FLAG_IGMP
        | NETIF_FLAG_BROADCAST
        | NETIF_FLAG_LINK_UP;

    // lwIP's doc: This function typically first resolves the hardware
    // address, then sends the packet.  For ethernet physical layer, this is
    // usually lwIP's etharp_output()
    _netif.output = etharp_output;

    // lwIP's doc: This function outputs the pbuf as-is on the link medium
    // (this must points to the raw ethernet driver, meaning: us)
    _netif.linkoutput = linkoutput_s;

    _netif.status_callback = netif_status_callback_s;

    return ERR_OK;
}

template <class RawDev>
void LwipIntfDev<RawDev>::netif_status_callback ()
{
    //XXX is it wise ?
    if (_default && connected())
        netif_set_default(&_netif);
    else if (netif_default == &_netif && !connected())
        netif_set_default(nullptr);
}

template <class RawDev>
err_t LwipIntfDev<RawDev>::handlePackets ()
{
    int pkt = 0;
    while(1)
    {
        if (++pkt == 10)
            // prevent starvation
            return ERR_OK;

        uint16_t tot_len = RawDev::readFrameSize();
        if (!tot_len)
            return ERR_OK;

        // from doc: use PBUF_RAM for TX, PBUF_POOL from RX
        // however:
        // PBUF_POOL can return chained pbuf (not in one piece)
        // and WiznetDriver does not have the proper API to deal with that
        // so in the meantime, we use PBUF_RAM instead which is currently
        // guarantying to deliver a continuous chunk of memory.
        // TODO: tweak the wiznet driver to allow copying partial chunk
        //       of received data and use PBUF_POOL.
        pbuf* pbuf = pbuf_alloc(PBUF_RAW, tot_len, PBUF_RAM);
        if (!pbuf || pbuf->len < tot_len)
        {
            if (pbuf)
                pbuf_free(pbuf);
            RawDev::discardFrame(tot_len);
            return ERR_BUF;
        }

        uint16_t len = RawDev::readFrameData((uint8_t*)pbuf->payload, tot_len);
        if (len != tot_len)
        {
            // tot_len is given by readFrameSize()
            // and is supposed to be honoured by readFrameData()
            // todo: ensure this test is unneeded, remove the print
            Serial.println("read error?\r\n");
            pbuf_free(pbuf);
            return ERR_BUF;
        }

        err_t err = _netif.input(pbuf, &_netif);

#if PHY_HAS_CAPTURE
        if (phy_capture)
            phy_capture(_netif.num, (const char*)pbuf->payload, tot_len, /*out*/0, /*success*/err == ERR_OK);
#endif

        if (err != ERR_OK)
        {
            pbuf_free(pbuf);
            return err;
        }
        // (else) allocated pbuf is now lwIP's responsibility

    }
}

template <class RawDev>
void LwipIntfDev<RawDev>::setDefault ()
{
    _default = true;
    if (connected())
        netif_set_default(&_netif);
}

#endif // _LWIPINTFDEV_H

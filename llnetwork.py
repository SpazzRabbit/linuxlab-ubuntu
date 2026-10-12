#!/usr/bin/python3
"""The network of the tablet, shown to GNOME the way NetworkManager would show it.

Android owns Wi-Fi and mobile data. An app, and Linux Lab is one, may use the connection, but it can
neither reach the radios nor change anything, so NetworkManager itself cannot work here. Without it
Settings says "NetworkManager needs to be running", the top bar has no network icon, and programs
that ask whether the computer is online get no answer.

This is the reading half of NetworkManager's D-Bus interface. It has one wired device, which stands
for whatever interface Android routes through at the moment, with the real addresses, gateway and
name servers, kept current. Think of the network card of a virtual machine: a ready connection whose
other end is somebody else's business. Every request to change something is refused.

Started by gnome-session.sh on the system bus of the session.
"""
import socket
import struct
import sys
import time
import uuid

import dbus
import dbus.service
from dbus.mainloop.glib import DBusGMainLoop
from gi.repository import GLib

NM = "org.freedesktop.NetworkManager"
ROOT = "/org/freedesktop/NetworkManager"
PROPERTIES = "org.freedesktop.DBus.Properties"
OBJECT_MANAGER = "org.freedesktop.DBus.ObjectManager"

DEVICE = ROOT + "/Devices/1"
ACTIVE = ROOT + "/ActiveConnection/1"
PROFILE = ROOT + "/Settings/1"
IP4 = ROOT + "/IP4Config/1"
IP6 = ROOT + "/IP6Config/1"
NOTHING = dbus.ObjectPath("/")

PROFILE_NAME = "Android network"
PROFILE_UUID = str(uuid.uuid5(uuid.NAMESPACE_DNS, "network.linuxlab-touchfix"))
PROFILE_TYPE = "802-3-ethernet"

# values of NetworkManager's enumerations
NM_STATE_DISCONNECTED, NM_STATE_CONNECTED = 20, 70
CONNECTIVITY_NONE, CONNECTIVITY_FULL = 1, 4
DEVICE_UNAVAILABLE, DEVICE_CHECKING, DEVICE_ACTIVATED = 20, 80, 100
REASON_NONE, REASON_CARRIER = 0, 40

u32 = dbus.UInt32


def paths(items):
    return dbus.Array([dbus.ObjectPath(item) for item in items], signature="o")


def strings(items):
    return dbus.Array(items, signature="s")


def variants(values):
    return dbus.Dictionary(values, signature="sv")


def dictionaries(items):
    return dbus.Array([variants(item) for item in items], signature="a{sv}")


class Refused(dbus.exceptions.DBusException):
    _dbus_error_name = NM + ".PermissionDenied"

    def __init__(self):
        super().__init__("Android owns the network of the tablet: nothing can be changed from Linux Lab")


# ---------------------------------------------------------------------------------------------
# What the kernel knows. /sys/class/net and most of /proc/net are closed to an Android app; rtnetlink
# answers.

RTM_GETLINK, RTM_GETADDR, RTM_GETROUTE = 18, 22, 26
NLMSG_ERROR, NLMSG_DONE = 2, 3


def dump(request, header):
    """The answers of the kernel to a dump request, without the netlink headers."""
    with socket.socket(socket.AF_NETLINK, socket.SOCK_RAW, 0) as s:
        s.settimeout(2)
        s.bind((0, 0))
        # NLM_F_REQUEST | NLM_F_DUMP
        s.send(struct.pack("=LHHLL", 16 + len(header), request, 0x301, 1, 0) + header)
        answers = []
        while True:
            data = s.recv(1 << 18)
            while len(data) >= 16:
                length, kind = struct.unpack_from("=LH", data)
                if kind == NLMSG_DONE or length < 16:
                    return answers
                if kind == NLMSG_ERROR:
                    raise OSError(-struct.unpack_from("=i", data, 16)[0], "rtnetlink refused the request")
                answers.append(data[16:length])
                data = data[(length + 3) & ~3:]


def attributes(data):
    found = {}
    while len(data) >= 4:
        length, kind = struct.unpack_from("=HH", data)
        if length < 4:
            break
        found[kind & 0x3FFF] = data[4:length]
        data = data[(length + 3) & ~3:]
    return found


def links():
    """Interfaces by index: name, hardware address, MTU."""
    found = {}
    for answer in dump(RTM_GETLINK, bytes(16)):
        index = struct.unpack_from("=BxHiII", answer)[2]
        a = attributes(answer[16:])
        if 3 not in a:  # IFLA_IFNAME
            continue
        mac = a.get(1, b"")  # IFLA_ADDRESS; Android hands apps zeros or nothing
        found[index] = {
            "name": a[3].split(b"\0")[0].decode(errors="replace"),
            "mac": ":".join("%02X" % byte for byte in mac) if len(mac) == 6 and any(mac) else "",
            "mtu": struct.unpack("=I", a[4])[0] if len(a.get(4, b"")) == 4 else 0,
        }
    return found


def addresses(family):
    """(interface index, address, prefix length) of every address of the family."""
    found = []
    for answer in dump(RTM_GETADDR, bytes([family]) + bytes(7)):
        its_family, prefix, _, _, index = struct.unpack_from("=BBBBI", answer)
        a = attributes(answer[8:])
        raw = a.get(2) or a.get(1)  # IFA_LOCAL, IFA_ADDRESS
        if its_family != family or not raw:
            continue
        try:
            found.append((index, socket.inet_ntop(family, raw), prefix))
        except (OSError, ValueError):
            pass
    return found


def gateway(family, index):
    """Where the default route of the interface goes; Android keeps it in a table of its own."""
    try:
        for answer in dump(RTM_GETROUTE, bytes([family]) + bytes(11)):
            its_family, destination_bits, _, _, _, _, _, kind = struct.unpack_from("=8B", answer)
            a = attributes(answer[12:])
            if its_family != family or destination_bits or kind != 1 or 5 not in a:  # RTN_UNICAST, RTA_GATEWAY
                continue
            if 4 in a and struct.unpack("=I", a[4])[0] != index:  # RTA_OIF
                continue
            return socket.inet_ntop(family, a[5])
    except (OSError, ValueError, struct.error):
        pass
    return ""


def source_address(family, target):
    """The address a packet to the Internet would leave from; None without a route. Sends nothing."""
    try:
        with socket.socket(family, socket.SOCK_DGRAM) as s:
            s.connect((target, 53))
            return s.getsockname()[0]
    except OSError:
        return None


def resolver():
    servers, searches = [], []
    try:
        with open("/etc/resolv.conf") as conf:
            for line in conf:
                words = line.split("#")[0].split()
                if len(words) >= 2 and words[0] == "nameserver":
                    servers.append(words[1].split("%")[0])
                elif len(words) >= 2 and words[0] in ("search", "domain"):
                    searches += words[1:]
    except OSError:
        pass
    return servers, searches


def is_address(family, text):
    try:
        socket.inet_pton(family, text)
        return True
    except (OSError, ValueError):
        return False


def look():
    """The connection of the tablet as it is now; None when there is none."""
    # documentation addresses: only the route to them matters
    source4 = source_address(socket.AF_INET, "192.0.2.1")
    source6 = source_address(socket.AF_INET6, "2001:db8::1")
    if not source4 and not source6:
        return None
    try:
        interfaces = links()
        all4 = addresses(socket.AF_INET)
        all6 = addresses(socket.AF_INET6)
    except (OSError, struct.error):
        interfaces, all4, all6 = {}, [], []
    index = next((i for i, address, _ in all4 if address == source4), None)
    if index is None:
        index = next((i for i, address, _ in all6 if address == source6), None)
    if index is None:
        # the kernel would not say: the address is all that is known
        return {
            "interface": "android",
            "mac": "",
            "mtu": 0,
            "addresses4": ((source4, 32),) if source4 else (),
            "addresses6": ((source6, 128),) if source6 else (),
            "gateway4": "",
            "gateway6": "",
            "servers": tuple(resolver()[0]),
            "searches": tuple(resolver()[1]),
        }
    # the address in use first
    mine4 = sorted(((a, p) for i, a, p in all4 if i == index), key=lambda item: item[0] != source4)
    mine6 = sorted(((a, p) for i, a, p in all6 if i == index), key=lambda item: item[0] != source6)
    link = interfaces.get(index, {})
    servers, searches = resolver()
    return {
        "interface": link.get("name") or socket.if_indextoname(index),
        "mac": link.get("mac", ""),
        "mtu": link.get("mtu", 0),
        "addresses4": tuple(mine4),
        "addresses6": tuple(mine6),
        "gateway4": gateway(socket.AF_INET, index) if mine4 else "",
        "gateway6": gateway(socket.AF_INET6, index) if mine6 else "",
        "servers": tuple(servers),
        "searches": tuple(searches),
    }


# ---------------------------------------------------------------------------------------------
# D-Bus objects


class Node(dbus.service.Object):
    """An object with properties."""

    def __init__(self, bus, path, interfaces):
        super().__init__(bus, path)
        self.path = path
        self.interfaces = interfaces  # {interface: {property: value}}

    @dbus.service.method(PROPERTIES, in_signature="ss", out_signature="v")
    def Get(self, interface, name):
        try:
            return self.interfaces[interface][name]
        except KeyError:
            raise dbus.exceptions.DBusException(
                "No property %s.%s" % (interface, name), name="org.freedesktop.DBus.Error.UnknownProperty"
            )

    @dbus.service.method(PROPERTIES, in_signature="s", out_signature="a{sv}")
    def GetAll(self, interface):
        return variants(self.interfaces.get(interface, {}))

    @dbus.service.method(PROPERTIES, in_signature="ssv")
    def Set(self, interface, name, value):
        raise Refused()

    @dbus.service.signal(PROPERTIES, signature="sa{sv}as")
    def PropertiesChanged(self, interface, changed, invalidated):
        pass

    def update(self, interface, values):
        """Sets properties and tells the clients about those that changed."""
        current = self.interfaces[interface]
        changed = {name: value for name, value in values.items() if name not in current or current[name] != value}
        if changed:
            current.update(changed)
            self.PropertiesChanged(interface, variants(changed), strings([]))


class Root(dbus.service.Object):
    """The object manager: libnm learns everything from it."""

    def __init__(self, bus):
        super().__init__(bus, "/org/freedesktop")
        self.nodes = {}

    @staticmethod
    def describe(node):
        return dbus.Dictionary(
            {interface: variants(values) for interface, values in node.interfaces.items()}, signature="sa{sv}"
        )

    @dbus.service.method(OBJECT_MANAGER, out_signature="a{oa{sa{sv}}}")
    def GetManagedObjects(self):
        return dbus.Dictionary(
            {dbus.ObjectPath(path): self.describe(node) for path, node in self.nodes.items()},
            signature="oa{sa{sv}}",
        )

    @dbus.service.signal(OBJECT_MANAGER, signature="oa{sa{sv}}")
    def InterfacesAdded(self, path, interfaces):
        pass

    @dbus.service.signal(OBJECT_MANAGER, signature="oas")
    def InterfacesRemoved(self, path, interfaces):
        pass

    def add(self, node):
        if node.path not in self.nodes:
            self.nodes[node.path] = node
            self.InterfacesAdded(dbus.ObjectPath(node.path), self.describe(node))

    def remove(self, node):
        if self.nodes.pop(node.path, None):
            self.InterfacesRemoved(dbus.ObjectPath(node.path), strings(list(node.interfaces)))


class Manager(Node):
    PERMISSIONS = (
        "enable-disable-network enable-disable-wifi enable-disable-wwan enable-disable-wimax sleep-wake "
        "network-control wifi.share.protected wifi.share.open settings.modify.system settings.modify.own "
        "settings.modify.hostname settings.modify.global-dns reload checkpoint-rollback "
        "enable-disable-statistics enable-disable-connectivity-check wifi.scan"
    ).split()

    def __init__(self, bus, refuse):
        self.refuse = refuse
        super().__init__(
            bus,
            ROOT,
            {
                NM: {
                    "Devices": paths([DEVICE]),
                    "AllDevices": paths([DEVICE]),
                    "Checkpoints": paths([]),
                    "NetworkingEnabled": True,
                    "WirelessEnabled": False,
                    "WirelessHardwareEnabled": False,
                    "WwanEnabled": False,
                    "WwanHardwareEnabled": False,
                    "RadioFlags": u32(0),
                    "ActiveConnections": paths([]),
                    "PrimaryConnection": NOTHING,
                    "PrimaryConnectionType": "",
                    "ActivatingConnection": NOTHING,
                    "Metered": u32(0),
                    "Startup": False,
                    "Version": "1.54.3",
                    "Capabilities": dbus.Array([], signature="u"),
                    "State": u32(NM_STATE_DISCONNECTED),
                    "Connectivity": u32(CONNECTIVITY_NONE),
                    "ConnectivityCheckAvailable": False,
                    "ConnectivityCheckEnabled": False,
                    "ConnectivityCheckUri": "",
                    "GlobalDnsConfiguration": variants({}),
                }
            },
        )

    @dbus.service.method(NM, out_signature="a{ss}")
    def GetPermissions(self):
        return dbus.Dictionary({NM + "." + name: "no" for name in self.PERMISSIONS}, signature="ss")

    @dbus.service.method(NM, out_signature="ao")
    def GetDevices(self):
        return self.interfaces[NM]["Devices"]

    @dbus.service.method(NM, out_signature="ao")
    def GetAllDevices(self):
        return self.interfaces[NM]["AllDevices"]

    @dbus.service.method(NM, in_signature="s", out_signature="o")
    def GetDeviceByIpIface(self, interface):
        return dbus.ObjectPath(DEVICE)

    @dbus.service.method(NM, out_signature="u")
    def state(self):
        return self.interfaces[NM]["State"]

    @dbus.service.method(NM, out_signature="u")
    def CheckConnectivity(self):
        return self.interfaces[NM]["Connectivity"]

    @dbus.service.method(NM, in_signature="ooo", out_signature="o")
    def ActivateConnection(self, connection, device, specific_object):
        # the one connection there is, is on whenever it can be
        if self.interfaces[NM]["ActiveConnections"]:
            return dbus.ObjectPath(ACTIVE)
        raise Refused()

    @dbus.service.method(NM, in_signature="a{sa{sv}}oo", out_signature="oo")
    def AddAndActivateConnection(self, connection, device, specific_object):
        raise Refused()

    @dbus.service.method(NM, in_signature="a{sa{sv}}ooa{sv}", out_signature="ooa{sv}")
    def AddAndActivateConnection2(self, connection, device, specific_object, options):
        raise Refused()

    @dbus.service.method(NM, in_signature="o")
    def DeactivateConnection(self, active_connection):
        raise self.refuse()

    @dbus.service.method(NM, in_signature="b")
    def Enable(self, enable):
        raise Refused()

    @dbus.service.method(NM, in_signature="b")
    def Sleep(self, sleep):
        raise Refused()

    @dbus.service.signal(NM, signature="u")
    def StateChanged(self, state):
        pass


class Settings(Node):
    def __init__(self, bus):
        super().__init__(
            bus,
            ROOT + "/Settings",
            {
                NM + ".Settings": {
                    "Connections": paths([PROFILE]),
                    "Hostname": socket.gethostname(),
                    "CanModify": False,
                }
            },
        )

    @dbus.service.method(NM + ".Settings", out_signature="ao")
    def ListConnections(self):
        return paths([PROFILE])

    @dbus.service.method(NM + ".Settings", in_signature="s", out_signature="o")
    def GetConnectionByUuid(self, wanted):
        if wanted == PROFILE_UUID:
            return dbus.ObjectPath(PROFILE)
        raise dbus.exceptions.DBusException("No such connection", name=NM + ".Settings.InvalidConnection")

    @dbus.service.method(NM + ".Settings", in_signature="a{sa{sv}}", out_signature="o")
    def AddConnection(self, connection):
        raise Refused()

    @dbus.service.method(NM + ".Settings", in_signature="a{sa{sv}}", out_signature="o")
    def AddConnectionUnsaved(self, connection):
        raise Refused()

    @dbus.service.method(NM + ".Settings", in_signature="a{sa{sv}}ua{sv}", out_signature="oa{sv}")
    def AddConnection2(self, connection, flags, arguments):
        raise Refused()

    @dbus.service.method(NM + ".Settings", in_signature="as", out_signature="bas")
    def LoadConnections(self, filenames):
        return True, strings([])

    @dbus.service.method(NM + ".Settings", out_signature="b")
    def ReloadConnections(self):
        return True

    @dbus.service.method(NM + ".Settings", in_signature="s")
    def SaveHostname(self, hostname):
        raise Refused()


class Profile(Node):
    """The saved connection behind the device; it says "automatic", which is the truth."""

    INTERFACE = NM + ".Settings.Connection"

    def __init__(self, bus):
        super().__init__(bus, PROFILE, {self.INTERFACE: {"Unsaved": False, "Flags": u32(0), "Filename": ""}})
        self.interface = ""

    def settings(self):
        connection = {"id": PROFILE_NAME, "uuid": PROFILE_UUID, "type": PROFILE_TYPE, "autoconnect": True}
        if self.interface:
            connection["interface-name"] = self.interface
        return dbus.Dictionary(
            {
                "connection": variants(connection),
                PROFILE_TYPE: variants({}),
                "ipv4": variants({"method": "auto"}),
                "ipv6": variants({"method": "auto"}),
            },
            signature="sa{sv}",
        )

    @dbus.service.method(INTERFACE, out_signature="a{sa{sv}}")
    def GetSettings(self):
        return self.settings()

    @dbus.service.method(INTERFACE, in_signature="s", out_signature="a{sa{sv}}")
    def GetSecrets(self, setting):
        return dbus.Dictionary({}, signature="sa{sv}")

    @dbus.service.method(INTERFACE, in_signature="a{sa{sv}}")
    def Update(self, properties):
        raise Refused()

    @dbus.service.method(INTERFACE, in_signature="a{sa{sv}}")
    def UpdateUnsaved(self, properties):
        raise Refused()

    @dbus.service.method(INTERFACE, in_signature="a{sa{sv}}ua{sv}", out_signature="a{sv}")
    def Update2(self, settings, flags, arguments):
        raise Refused()

    @dbus.service.method(INTERFACE)
    def Delete(self):
        raise Refused()

    @dbus.service.method(INTERFACE)
    def Save(self):
        raise Refused()

    @dbus.service.method(INTERFACE)
    def ClearSecrets(self):
        pass

    @dbus.service.signal(INTERFACE, signature="")
    def Updated(self):
        pass


class Device(Node):
    INTERFACE = NM + ".Device"
    WIRED = NM + ".Device.Wired"

    def __init__(self, bus, profile, refuse):
        super().__init__(
            bus,
            DEVICE,
            {
                self.INTERFACE: {
                    "Udi": "/sys/devices/virtual/net/android",
                    "Path": "",
                    "Interface": "",
                    "IpInterface": "",
                    "Driver": "android",
                    "DriverVersion": "",
                    "FirmwareVersion": "",
                    "Capabilities": u32(3),  # supported, detects the carrier
                    "State": u32(DEVICE_UNAVAILABLE),
                    "StateReason": dbus.Struct((u32(DEVICE_UNAVAILABLE), u32(REASON_CARRIER)), signature="uu"),
                    "ActiveConnection": NOTHING,
                    "Ip4Config": dbus.ObjectPath(IP4),
                    "Dhcp4Config": NOTHING,
                    "Ip6Config": dbus.ObjectPath(IP6),
                    "Dhcp6Config": NOTHING,
                    "Managed": True,
                    "Autoconnect": True,
                    "FirmwareMissing": False,
                    "NmPluginMissing": False,
                    "DeviceType": u32(1),  # Ethernet
                    "AvailableConnections": paths([PROFILE]),
                    "PhysicalPortId": "",
                    "Mtu": u32(0),
                    "Metered": u32(0),
                    "LldpNeighbors": dictionaries([]),
                    "Real": True,
                    "Ip4Connectivity": u32(CONNECTIVITY_NONE),
                    "Ip6Connectivity": u32(CONNECTIVITY_NONE),
                    "InterfaceFlags": u32(0),
                    "HwAddress": "",
                    "Ports": paths([]),
                },
                self.WIRED: {
                    "HwAddress": "",
                    "PermHwAddress": "",
                    "Speed": u32(0),
                    "S390Subchannels": strings([]),
                    "Carrier": False,
                },
            },
        )
        self.profile = profile
        self.refuse = refuse

    @dbus.service.method(INTERFACE)
    def Disconnect(self):
        raise self.refuse()

    @dbus.service.method(INTERFACE)
    def Delete(self):
        raise Refused()

    @dbus.service.method(INTERFACE, in_signature="a{sa{sv}}tu")
    def Reapply(self, connection, version, flags):
        raise Refused()

    @dbus.service.method(INTERFACE, in_signature="u", out_signature="a{sa{sv}}t")
    def GetAppliedConnection(self, flags):
        return self.profile.settings(), dbus.UInt64(0)

    @dbus.service.signal(INTERFACE, signature="uuu")
    def StateChanged(self, new, old, reason):
        pass


class Agents(Node):
    """GNOME Shell offers to ask for passwords; nobody will ever need one."""

    INTERFACE = NM + ".AgentManager"

    def __init__(self, bus):
        super().__init__(bus, ROOT + "/AgentManager", {self.INTERFACE: {}})

    @dbus.service.method(INTERFACE, in_signature="s")
    def Register(self, identifier):
        pass

    @dbus.service.method(INTERFACE, in_signature="su")
    def RegisterWithCapabilities(self, identifier, capabilities):
        pass

    @dbus.service.method(INTERFACE)
    def Unregister(self):
        pass


class Network:
    def __init__(self, bus):
        self.root = Root(bus)
        self.manager = Manager(bus, self.refuse_off)
        self.settings = Settings(bus)
        self.profile = Profile(bus)
        self.device = Device(bus, self.profile, self.refuse_off)
        self.ip4 = Node(bus, IP4, {NM + ".IP4Config": {}})
        self.ip6 = Node(bus, IP6, {NM + ".IP6Config": {}})
        self.dns = Node(bus, ROOT + "/DnsManager", {NM + ".DnsManager": {}})
        self.active = Node(
            bus,
            ACTIVE,
            {
                NM + ".Connection.Active": {
                    "Connection": dbus.ObjectPath(PROFILE),
                    "SpecificObject": NOTHING,
                    "Id": PROFILE_NAME,
                    "Uuid": PROFILE_UUID,
                    "Type": PROFILE_TYPE,
                    "Devices": paths([DEVICE]),
                    "State": u32(2),  # activated
                    "StateFlags": u32(4 | 8 | 16),  # layer 2, IPv4 and IPv6 are ready
                    "Default": False,
                    "Ip4Config": dbus.ObjectPath(IP4),
                    "Dhcp4Config": NOTHING,
                    "Default6": False,
                    "Ip6Config": dbus.ObjectPath(IP6),
                    "Dhcp6Config": NOTHING,
                    "Vpn": False,
                    "Controller": NOTHING,
                    "Master": NOTHING,
                }
            },
        )
        self.seen = False  # not None: the first look is always shown
        self.pending = 0
        self.show(look())
        for node in (self.manager, self.settings, self.profile, self.device, self.ip4, self.ip6, self.dns, Agents(bus)):
            self.root.nodes.setdefault(node.path, node)

    def show(self, net):
        if net == self.seen:
            return
        was_connected = bool(self.seen)
        self.seen = net
        connected = net is not None
        net = net or {}
        servers = net.get("servers", ())
        servers4 = [s for s in servers if is_address(socket.AF_INET, s)]
        servers6 = [s for s in servers if is_address(socket.AF_INET6, s)]
        searches = strings(list(net.get("searches", ())))
        addresses4 = net.get("addresses4", ())
        addresses6 = net.get("addresses6", ())

        self.ip4.update(
            NM + ".IP4Config",
            {
                "AddressData": dictionaries([{"address": a, "prefix": u32(p)} for a, p in addresses4]),
                "Addresses": dbus.Array([], signature="au"),
                "Gateway": net.get("gateway4", ""),
                "RouteData": dictionaries([]),
                "Routes": dbus.Array([], signature="au"),
                "NameserverData": dictionaries([{"address": s} for s in servers4]),
                "Nameservers": dbus.Array(
                    [u32(struct.unpack("=I", socket.inet_aton(s))[0]) for s in servers4], signature="u"
                ),
                "Domains": searches,
                "Searches": searches,
                "DnsOptions": strings([]),
                "DnsPriority": dbus.Int32(100),
                "WinsServerData": strings([]),
                "WinsServers": dbus.Array([], signature="u"),
            },
        )
        self.ip6.update(
            NM + ".IP6Config",
            {
                "AddressData": dictionaries([{"address": a, "prefix": u32(p)} for a, p in addresses6]),
                "Addresses": dbus.Array([], signature="(ayuay)"),
                "Gateway": net.get("gateway6", ""),
                "RouteData": dictionaries([]),
                "Routes": dbus.Array([], signature="(ayuayu)"),
                "Nameservers": dbus.Array(
                    [dbus.ByteArray(socket.inet_pton(socket.AF_INET6, s)) for s in servers6], signature="ay"
                ),
                "Domains": searches,
                "Searches": searches,
                "DnsOptions": strings([]),
                "DnsPriority": dbus.Int32(100),
            },
        )
        configuration = [{"nameservers": strings(list(servers)), "priority": dbus.Int32(100)}] if servers else []
        if servers and net.get("interface"):
            configuration[0]["interface"] = net["interface"]
        self.dns.update(
            NM + ".DnsManager",
            {"Mode": "none", "RcManager": "unmanaged", "Configuration": dictionaries(configuration)},
        )

        interface = net.get("interface", self.profile.interface)
        if interface != self.profile.interface:
            self.profile.interface = interface
            self.profile.Updated()

        state = DEVICE_ACTIVATED if connected else DEVICE_UNAVAILABLE
        reason = REASON_NONE if connected else REASON_CARRIER
        connectivity = u32(CONNECTIVITY_FULL if connected else CONNECTIVITY_NONE)
        if connected:
            self.active.update(
                NM + ".Connection.Active", {"Default": bool(addresses4), "Default6": bool(net.get("gateway6"))}
            )
            self.root.add(self.active)
        old = self.device.interfaces[Device.INTERFACE]["State"]
        self.device.update(
            Device.INTERFACE,
            {
                "Udi": "/sys/devices/virtual/net/" + (interface or "android"),
                "Interface": interface,
                "IpInterface": interface if connected else "",
                "State": u32(state),
                "StateReason": dbus.Struct((u32(state), u32(reason)), signature="uu"),
                "ActiveConnection": dbus.ObjectPath(ACTIVE) if connected else NOTHING,
                "Mtu": u32(net.get("mtu", 0)),
                "Ip4Connectivity": connectivity if addresses4 else u32(CONNECTIVITY_NONE),
                "Ip6Connectivity": connectivity if net.get("gateway6") else u32(CONNECTIVITY_NONE),
                # up, has a carrier
                "InterfaceFlags": u32(0x10003 if connected else 0),
                "HwAddress": net.get("mac", ""),
            },
        )
        self.device.update(
            Device.WIRED,
            {"HwAddress": net.get("mac", ""), "PermHwAddress": net.get("mac", ""), "Carrier": connected},
        )
        if old != state:
            self.device.StateChanged(u32(state), u32(old), u32(reason))
        self.manager.update(
            NM,
            {
                "ActiveConnections": paths([ACTIVE] if connected else []),
                "PrimaryConnection": dbus.ObjectPath(ACTIVE) if connected else NOTHING,
                "PrimaryConnectionType": PROFILE_TYPE if connected else "",
                "State": u32(NM_STATE_CONNECTED if connected else NM_STATE_DISCONNECTED),
                "Connectivity": connectivity,
            },
        )
        if connected != was_connected:
            self.manager.StateChanged(u32(NM_STATE_CONNECTED if connected else NM_STATE_DISCONNECTED))
        if not connected:
            self.root.remove(self.active)
        print(
            "[llnetwork]",
            "%s %s" % (interface, " ".join(a for a, _ in addresses4 + addresses6)) if connected else "no connection",
            flush=True,
        )

    def refuse_off(self):
        """The answer to "switch the connection off".

        Settings and the top bar flip their switch before they ask, and put it back only when the
        device reports a new state. So the state is reported once more: connecting for a moment, then
        connected, which is how a network card answers that could not be switched off.
        """

        def announce(new, old):
            self.device.update(
                Device.INTERFACE,
                {"State": u32(new), "StateReason": dbus.Struct((u32(new), u32(REASON_NONE)), signature="uu")},
            )
            self.device.StateChanged(u32(new), u32(old), u32(REASON_NONE))
            return False

        if self.seen and self.device.interfaces[Device.INTERFACE]["State"] == DEVICE_ACTIVATED:
            GLib.idle_add(announce, DEVICE_CHECKING, DEVICE_ACTIVATED)
            GLib.timeout_add(400, announce, DEVICE_ACTIVATED, DEVICE_CHECKING)
        return Refused()

    def refresh(self):
        self.pending = 0
        try:
            self.show(look())
        except Exception as error:  # a daemon that dies takes the network icon with it
            print("[llnetwork] could not look at the network:", repr(error), flush=True)
        return False

    def changed(self, *_):
        """Something about the network changed: look again when the burst of news is over."""
        if not self.pending:
            self.pending = GLib.timeout_add(500, self.refresh)
        return True


def watch(network):
    """Calls network.changed() when the kernel reports a change of links, addresses or routes."""
    try:
        s = socket.socket(socket.AF_NETLINK, socket.SOCK_RAW, 0)
        # RTMGRP_LINK, IPV4_IFADDR, IPV4_ROUTE, IPV6_IFADDR, IPV6_ROUTE
        s.bind((0, 0x1 | 0x10 | 0x40 | 0x100 | 0x400))
        s.setblocking(False)
    except OSError as error:
        print("[llnetwork] no news from the kernel (%s), looking every few seconds" % error, flush=True)
        return None

    def news(*_):
        try:
            while s.recv(1 << 16):
                pass
        except OSError:
            pass
        return network.changed()

    GLib.io_add_watch(s.fileno(), GLib.IO_IN, news)
    return s


def main():
    DBusGMainLoop(set_as_default=True)
    # the session starts the bus a moment before this
    for attempt in range(50):
        try:
            bus = dbus.SystemBus()
            break
        except dbus.exceptions.DBusException:
            time.sleep(0.2)
    else:
        print("[llnetwork] no system bus", flush=True)
        return 1
    network = Network(bus)
    # the objects are there before the name: a client that sees the name asks at once
    if bus.request_name(NM, dbus.bus.NAME_FLAG_DO_NOT_QUEUE) != dbus.bus.REQUEST_NAME_REPLY_PRIMARY_OWNER:
        print("[llnetwork] the name is taken: another copy, or a real NetworkManager", flush=True)
        return 0
    keep = watch(network)  # noqa: F841 (the socket lives as long as the loop)
    # a change the kernel does not report: /etc/resolv.conf; and the fallback when it reports nothing
    GLib.timeout_add_seconds(20 if keep else 5, lambda: network.changed())
    GLib.MainLoop().run()
    return 0


if __name__ == "__main__":
    sys.exit(main())

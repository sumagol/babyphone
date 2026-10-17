use std::net::{IpAddr, Ipv4Addr, SocketAddrV4};

use anyhow::{anyhow, Context, Result};
use socket2::{Domain, Protocol, SockAddr, Socket, Type};

pub const TARGET: SocketAddrV4 = SocketAddrV4::new(Ipv4Addr::new(239, 255, 0, 1), 5004);

pub struct MulticastSender {
    socket: Socket,
    target: SockAddr,
}

impl MulticastSender {
    pub fn new(iface: Option<&str>, ttl: u32) -> Result<Self> {
        let socket = Socket::new(Domain::IPV4, Type::DGRAM, Some(Protocol::UDP))?;
        socket.set_multicast_ttl_v4(ttl)?;
        // Lets a receiver on this same machine hear the stream too.
        socket.set_multicast_loop_v4(true)?;

        match iface {
            Some(spec) => {
                let ip = resolve_iface(spec)?;
                // Pin egress so packets don't leave through a VPN or docker bridge.
                socket.set_multicast_if_v4(&ip)?;
                socket.bind(&SockAddr::from(SocketAddrV4::new(ip, 0)))?;
                eprintln!("Interface: {spec} ({ip}), TTL {ttl}");
            }
            None => eprintln!("Interface: OS default route, TTL {ttl} (use --iface if a VPN is active)"),
        }

        Ok(Self { socket, target: SockAddr::from(TARGET) })
    }

    pub fn send(&self, packet: &[u8]) -> std::io::Result<usize> {
        self.socket.send_to(packet, &self.target)
    }
}

fn resolve_iface(spec: &str) -> Result<Ipv4Addr> {
    if let Ok(ip) = spec.parse::<Ipv4Addr>() {
        return Ok(ip);
    }
    if_addrs::get_if_addrs()
        .context("listing network interfaces")?
        .into_iter()
        .filter(|i| i.name == spec)
        .find_map(|i| match i.ip() {
            IpAddr::V4(v4) => Some(v4),
            IpAddr::V6(_) => None,
        })
        .ok_or_else(|| anyhow!("interface {spec:?} not found or has no IPv4 address, see --list"))
}

pub fn list_interfaces() -> Result<()> {
    println!("Network interfaces (for --iface):");
    for i in if_addrs::get_if_addrs()? {
        if let IpAddr::V4(ip) = i.ip() {
            if !i.is_loopback() {
                println!("    {:<16} {ip}", i.name);
            }
        }
    }
    Ok(())
}

# Newrex OS

Newrex is a hobby x86 operating system written in C.

## Features

* Custom RexFS filesystem
* NEX executable format
* Heap allocator
* Physical and virtual memory management
* PCI enumeration
* RTL8139 networking
* ARP
* IPv4
* ICMP Ping
* Interactive shell

## Current Status

Developer Preview (v0.1.0-beta)

## Screenshots



## Building

```bash
make -f kernel/Makefile clean all iso
```

## Running

```bash
./boot.sh
```

## Roadmap

* Scheduler
* Ring 3 user mode
* TCP
* DHCP
* HTTP client

## License

MIT License

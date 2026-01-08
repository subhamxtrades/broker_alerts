# OSPF Simulator

This document provides a detailed overview of the OSPF simulator, including its architecture, configuration, and usage instructions.

## Architecture

The OSPF simulator is designed to support the simulation of multiple OSPF routers, each with its own set of virtual interfaces. This allows for the creation of complex network topologies for testing and analysis.

### Multi-Router Simulation

The simulator is built around a top-level `ospf_simulator` data structure, which holds an array of `ospf_router_instance` structures. Each `ospf_router_instance` represents a single simulated OSPF router, with its own unique router ID and a list of virtual interfaces.

### Virtual Interfaces

A virtual interface (`ospf_virtual_interface`) is a logical OSPF interface that is mapped to a physical DPDK port. This allows multiple simulated routers to send and receive OSPF packets on the same physical network interface. Each virtual interface has its own unique MAC address, IP address, and network mask, which are used to differentiate it from other virtual interfaces on the same port.

### Data Structures

The main data structures used in the simulator are:

-   `ospf_simulator`: The top-level data structure that holds all the simulated router instances.
-   `ospf_router_instance`: Represents a single simulated OSPF router, with its own router ID, LSDB, and a list of virtual interfaces.
-   `ospf_virtual_interface`: Represents a logical OSPF interface, with its own MAC address, IP address, and network mask.
-   `ospf_neighbor`: Represents an OSPF neighbor, with its own state machine, retransmission list, and other neighbor-specific data.
-   `lsdb_entry`: Represents a single LSA in the Link-State Database.

## Configuration

The OSPF simulator is configured using a simple, line-based configuration file named `ospf_sim.conf`. This file allows you to define the number of routers to simulate, their router IDs, and the properties of their virtual interfaces.

### File Format

The configuration file is organized into sections, with each section starting with a section header in square brackets (e.g., `[router]`). The following sections are supported:

-   `[router]`: Defines a new OSPF router instance.
-   `[interface]`: Defines a new virtual interface for the current router instance.

Within each section, key-value pairs are used to specify the configuration parameters. The following parameters are supported:

**`[router]` section:**

-   `router_id`: The OSPF router ID for this router instance, in dotted-decimal notation.

**`[interface]` section:**

-   `port`: The physical DPDK port number that this virtual interface should be mapped to.
-   `mac`: The MAC address for this virtual interface, in the format `XX:XX:XX:XX:XX:XX`.
-   `ip`: The IP address for this virtual interface, in dotted-decimal notation.
-   `netmask`: The network mask for this virtual interface, in dotted-decimal notation.

### Example Configuration

Here is an example `ospf_sim.conf` file that defines a single router with two virtual interfaces:

```
# OSPF Simulator Configuration

[router]
router_id=2.2.2.2

[interface]
port=0
mac=00:00:00:00:00:01
ip=192.168.1.100
netmask=255.255.255.0

[interface]
port=1
mac=00:00:00:00:00:02
ip=192.168.2.100
netmask=255.255.255.0
```

## Build and Run Instructions

This section provides instructions on how to compile and run the OSPF simulator.

### Prerequisites

-   **DPDK:** The Data Plane Development Kit (DPDK) must be installed and configured on your system. You can find instructions on how to do this on the [DPDK website](https://doc.dpdk.org/guides/linux_gsg/intro.html).
-   **GCC:** The GNU Compiler Collection (GCC) is required to compile the simulator.

### Compilation

To compile the simulator, you will need to set the `RTE_SDK` and `RTE_TARGET` environment variables to point to your DPDK installation. Then, you can use the following `gcc` command to compile the code:

```bash
export RTE_SDK=/path/to/your/dpdk
export RTE_TARGET=x86_64-native-linuxapp-gcc

gcc -O3 -I${RTE_SDK}/${RTE_TARGET}/include -include ${RTE_SDK}/${RTE_TARGET}/include/rte_config.h -L${RTE_SDK}/${RTE_TARGET}/lib -Wl,-rpath=${RTE_SDK}/${RTE_TARGET}/lib -o route_storm_ospf route_storm_ospf.c -ldpdk -lrt -lm -ldl
```

### Running the Simulator

To run the simulator, you will need to use the `dpdk-devbind.py` script to bind your network interfaces to the DPDK driver. Then, you can run the simulator with the appropriate EAL arguments.

1.  **Bind network interfaces:**

    ```bash
    cd ${RTE_SDK}
    sudo usertools/dpdk-devbind.py --bind=igb_uio enp0s8 enp0s9
    ```

2.  **Run the simulator:**

    ```bash
    sudo ./route_storm_ospf -l 0-1 -n 4 -- -p 0x3
    ```

## FRR Configuration

This section provides a sample `frr.conf` configuration to demonstrate how to set up two FRR routers to peer with the OSPF simulator instances defined in the `ospf_sim.conf` file.

### FRR Router 1

```
!
! Zebra configuration file
!
hostname frr1
password zebra
enable password zebra
!
! OSPF Configuration
!
interface enp0s8
 ip ospf area 0
!
router ospf
 ospf router-id 1.1.1.1
 network 192.168.1.0/24 area 0
!
```

### FRR Router 2

```
!
! Zebra configuration file
!
hostname frr2
password zebra
enable password zebra
!
! OSPF Configuration
!
interface enp0s9
 ip ospf area 0
!
router ospf
 ospf router-id 3.3.3.3
 network 192.168.2.0/24 area 0
!
```

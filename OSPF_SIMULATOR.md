# OSPF Simulator Documentation

This document provides an overview of the OSPF simulator's architecture, code flow, and operational details, along with a sample configuration for peering with an FRR router.

## 1. Architecture

The OSPF simulator is a C-based application designed to run on top of the Data Plane Development Kit (DPDK) for high-performance networking. Its primary purpose is to simulate one or more OSPFv2 routers that can establish adjacencies and exchange routing information with standard routers like FRRouting (FRR).

The architecture is designed to be simple and scalable, with each simulated OSPF router instance tied to a specific DPDK port (identified by a `pid`).

### Core Data Structures

The state and configuration of the simulator are managed through a set of key data structures defined in `route_storm_ospf.h`:

*   **`ospf_session_t`**: This is the central data structure for a simulated OSPF router instance. A global array, `ospf_sessions[RTE_MAX_ETHPORTS]`, stores one session for each potential DPDK port. Each `ospf_session_t` contains:
    *   The **Router ID** and **Area ID**.
    *   An array of `ospf_neighbor_t` structures to track discovered neighbors.
    *   An array of `ospf_interface_t` structures representing the router's network interfaces.
    *   Pointers to the **Link-State Database (LSDB)**, **Routing Information Base (RIB)**, and **Forwarding Information Base (FIB)**, which are implemented using DPDK's `rte_hash` for efficient lookups.

*   **`ospf_neighbor_t`**: This structure maintains the state of a single OSPF neighbor. It stores critical information for the adjacency state machine, including:
    *   The neighbor's **Router ID**, IP address, and MAC address.
    *   The current adjacency `state` (e.g., `OSPF_STATE_INIT`, `OSPF_STATE_EXCHANGE`, `OSPF_STATE_FULL`).
    *   Sequence numbers for Database Description (DD) packets.
    *   Timers, such as `last_hello_received`, to detect neighbor timeouts.

*   **`ospf_interface_t`**: This structure defines the properties of a network interface on the simulated router. In the current implementation, all interfaces are configured as **Point-to-Point (P2P)**, which simplifies the OSPF protocol by eliminating the need for a Designated Router (DR) election.

### State Management

The simulator operates in a single-threaded loop for each logical core, where each core can manage one or more DPDK ports. The state of each simulated router (`ospf_session_t`) is completely independent of the others, allowing for clear separation and stable operation.

State transitions are driven by two primary mechanisms:
1.  **Incoming Packets**: The `ospf_process_packet` function acts as the main dispatcher, directing incoming OSPF packets to the appropriate handler (e.g., `ospf_handle_hello_packet`, `ospf_handle_dd_packet`) based on their type.
2.  **Internal Timers**: The main loop periodically calls functions like `ospf_process_neighbor_timeouts` to check for dead neighbors and `ospf_send_hello_packet` to maintain adjacencies.

## 2. Code Flow

The execution of the OSPF simulator is managed by the `ospf_test_main_loop` function, which contains the primary event loop.

### Initialization

Before the main loop begins, the simulator sets up the environment for a given DPDK port (`pid`):

1.  **`ospf_initialize_test(pid, router_id, area_id)`**: This function is called to initialize the OSPF session.
    *   It first calls `ospf_cleanup_session` to clear any previous state.
    *   It configures the `ospf_session_t` with the router's static properties, such as its **Router ID**, **Area ID**, and default Hello/Dead intervals.
    *   It initializes the network interface (`ospf_interface_t`) as a **Point-to-Point** link with a hardcoded IP address.
    *   It creates the `rte_hash` tables that will serve as the **LSDB**, **RIB**, and **FIB**.

2.  **Initial Hello Packet**: Immediately after initialization, the simulator sends its first **Hello packet** via `ospf_send_hello_packet`. This is a critical step to announce its presence on the network and begin the neighbor discovery process.

### Main Event Loop

The `while (pblast[pid].trafficStatus)` loop is the heart of the simulator. On each iteration, it performs the following tasks in sequence:

1.  **Process Timers and Timeouts**:
    *   **Hello Timer**: It checks if the `hello_interval` has passed since the last Hello was sent. If so, it calls `ospf_send_hello_packet` to maintain neighbor relationships.
    *   **Neighbor Timeout**: It calls `ospf_process_neighbor_timeouts` to check if the `dead_interval` has expired for any neighbors. If a neighbor is declared dead, its state is set to `OSPF_STATE_DOWN`, and it is removed from the neighbor table.

2.  **Receive and Process Packets**:
    *   `rte_eth_rx_burst` is called to poll the DPDK port for incoming packets.
    *   Each received packet is checked to see if it is an OSPF packet (IP protocol number 89).
    *   Valid OSPF packets are passed to the `ospf_process_packet` function for handling.

### Packet Processing Pipeline

The `ospf_process_packet` function is the entry point for all received OSPF traffic. It performs the following steps:

1.  **Checksum Validation**: It calculates the OSPF checksum of the received packet using `ospf_checksum` and compares it to the checksum in the header. **If the checksum is invalid, the packet is discarded immediately.**
2.  **Dispatch to Handler**: Based on the `type` field in the OSPF header, a `switch` statement dispatches the packet to the appropriate handler function:
    *   `ospf_handle_hello_packet`
    *   `ospf_handle_dd_packet`
    *   `ospf_handle_lsr_packet`
    *   `ospf_handle_lsu_packet`
    *   `ospf_handle_lsack_packet`
3.  **State Machine Update**: Each handler function contains the logic for processing a specific OSPF packet type and advancing the neighbor state machine. For example, `ospf_handle_hello_packet` is responsible for transitioning a neighbor from `DOWN` to `INIT` or `2-WAY`, while the other handlers manage the transition through `EXSTART`, `EXCHANGE`, `LOADING`, and `FULL`.

## 3. How it Works: The OSPF Adjacency Process

The simulator follows the standard OSPFv2 state machine to establish an adjacency with a peer router. Because all interfaces are configured as **Point-to-Point**, the process is simplified and does not involve a DR/BDR election.

1.  **Down State**: The initial state. The simulator begins sending Hello packets to the `AllSPFRouters` multicast address (`224.0.0.5`) to discover neighbors.

2.  **Init State**: When the simulator receives a Hello packet from a new neighbor (e.g., an FRR router), it creates an `ospf_neighbor_t` entry for that neighbor and transitions its state to `INIT`. In this state, the simulator has heard from the neighbor, but two-way communication has not yet been established. The simulator continues to send Hellos, now including the Router ID of the newly discovered neighbor in the "Neighbors" list of its Hello packets.

3.  **2-Way State**: When the simulator receives a Hello packet from a neighbor that contains the simulator's own Router ID in the "Neighbors" list, it confirms that communication is bidirectional. The state transitions to `2-WAY`.

4.  **ExStart State**: For Point-to-Point links, the state machine immediately proceeds from `2-WAY` to `EXSTART`. In this state, the two routers negotiate the master/slave relationship for the upcoming LSDB synchronization.
    *   **Master/Slave Election**: The router with the **higher Router ID** becomes the **master**. The master is responsible for setting the initial sequence number for the Database Description (DD) packet exchange.
    *   The `ospf_handle_hello_packet` function determines the master/slave role. If the simulator is the master, it sends the first DD packet with the `I` (Initial), `M` (More), and `MS` (Master/Slave) bits set.

5.  **Exchange State**: The routers exchange a series of DD packets to summarize their Link-State Databases (LSDBs).
    *   Each DD packet contains a set of LSA headers.
    *   The master increments the DD sequence number for each new packet, and the slave acknowledges by echoing the sequence number.
    *   The `M` (More) bit is set in all but the final DD packet.
    *   The `ospf_handle_dd_packet` function manages this exchange, sending and acknowledging DD packets until both routers have a complete picture of the other's LSDB summary.

6.  **Loading State**: After the DD exchange is complete, each router knows which LSAs it is missing from its peer. The state transitions to `LOADING`.
    *   The simulator sends **Link-State Request (LSR)** packets to request the full details of any missing or outdated LSAs.
    *   The peer router responds with **Link-State Update (LSU)** packets, which contain the full LSA data.
    *   The simulator acknowledges the receipt of the LSU with a **Link-State Acknowledgment (LSAck)** packet.
    *   This process continues until all requested LSAs have been received.

7.  **Full State**: Once the LSDBs of the two routers are fully synchronized, the neighbor state transitions to `FULL`. The adjacency is complete, and the routers can now include each other in their SPF (Shortest Path First) calculations.

## 4. Sample FRR Configuration

This section provides a sample configuration for an FRR router to establish an OSPF adjacency with the simulator. This configuration should be applied using FRR's integrated shell, `vtysh`.

### Assumptions

*   The FRR router has an interface (e.g., `eth1`) connected to the same Layer 2 network as the DPDK port `0` used by the simulator.
*   The simulator is running on DPDK port `0`, which is configured with the IP address `192.168.1.100/24`.
*   The FRR router's interface will be configured with the IP address `192.168.1.1/24`.

### Configuration Steps

```shell
# Enter configuration mode
configure terminal

# Configure the network interface
interface eth1
 ip address 192.168.1.1/24
!
# Enter OSPF router configuration mode
router ospf
 # Set the OSPF Router ID. This must match what the simulator expects.
 ospf router-id 1.1.1.1
 # Announce the directly connected network. This enables OSPF on the interface.
 network 192.168.1.0/24 area 0.0.0.0
!
# (Optional) Set the interface's network type to point-to-point
# This should be detected automatically from the simulator's Hello packets,
# but can be set explicitly for clarity.
interface eth1
 ip ospf network point-to-point
!
# Exit configuration mode
end

# (Optional) Save the configuration
write
```

### Verification

Once configured, you can verify the OSPF adjacency on the FRR router using the following commands in `vtysh`:

*   **Check neighbor status**:
    ```shell
    show ip ospf neighbor
    ```
    The output should show the simulator's Router ID (`2.2.2.2`) in the `FULL` state.

*   **Check the OSPF interface**:
    ```shell
    show ip ospf interface eth1
    ```
    This command will show detailed information about the OSPF configuration on the interface, including the network type, timers, and neighbor count.

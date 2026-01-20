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
    *   It configures the `ospf_session_t` with the router's static properties, such as its **Router ID**, **Area ID**, and the new, more aggressive Hello/Dead intervals.
    *   It initializes the network interface (`ospf_interface_t`) as a **Point-to-Point** link with a hardcoded IP address and a `/30` subnet mask.
    *   It creates the `rte_hash` tables that will serve as the **LSDB**, **RIB**, and **FIB**.

2.  **Initial Hello Packet**: Immediately after initialization, the simulator sends its first **Hello packet** via `ospf_send_hello_packet`. This is a critical step to announce its presence on the network and begin the neighbor discovery process.

### Main Event Loop

The `while (pblast[pid].trafficStatus)` loop is the heart of the simulator. On each iteration, it performs the following tasks in sequence:

1.  **Process Timers and Timeouts**:
    *   **Preemptive Hello Timer**: It checks if 90% of the `hello_interval` (1.8 seconds) has passed since the last Hello was sent. If so, it calls `ospf_send_hello_packet` to proactively compensate for network and kernel delays.
    *   **Neighbor Timeout**: It calls `ospf_process_neighbor_timeouts` to check if the `dead_interval` (plus a 500ms grace period) has expired for any neighbors.

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

The simulator follows the standard OSPFv2 state machine to establish an adjacency with a peer router. Because all interfaces are configured as **Point-to-Point**, the process is simplified and does not involve a DR/BDR election. The state machine progresses from `DOWN` to `INIT`, `2-WAY`, `EXSTART`, `EXCHANGE`, `LOADING`, and finally `FULL`.

## 4. Sample FRR Configuration (Dual-Port)

This section provides a sample configuration for an FRR router to establish OSPF adjacencies with the DPDK simulator, incorporating the new, more aggressive timing parameters.

### Assumptions

This configuration assumes the following network topology:

*   **Link 1**:
    *   FRR `ens37` IP: `192.168.1.1/30`.
    *   DPDK Port 0 IP: `192.168.1.2/30` (Router ID: `2.2.2.2`).
*   **Link 2**:
    *   FRR `ens38` IP: `192.168.2.1/30`.
    *   DPDK Port 1 IP: `192.168.2.2/30` (Router ID: `2.2.2.2`).
*   The FRR router itself has a Router ID of `1.1.1.1`.

### FRR Configuration

```shell
# Enter configuration mode
configure terminal

# --- Configure Interfaces ---
interface ens37
  ip address 192.168.1.1/30
  ip ospf network point-to-point
  ip ospf hello-interval 3
  ip ospf dead-interval 12
  ip ospf retransmit-interval 2
  ip ospf mtu-ignore
  ip ospf priority 255
exit
!
interface ens38
  ip address 192.168.2.1/30
  ip ospf network point-to-point
  ip ospf hello-interval 3
  ip ospf dead-interval 12
  ip ospf retransmit-interval 2
  ip ospf mtu-ignore
  ip ospf priority 0
exit
!

# --- Configure OSPF Process ---
router ospf
  router-id 1.1.1.1
  network 192.168.1.0/30 area 0
  network 192.168.2.0/30 area 0
exit
!

# Exit configuration mode
end
```

> **Note on Interface Priority**: The use of different OSPF priorities (`255` and `0`) is a critical part of this configuration. Because the simulator uses a non-standard duplicate Router ID (`2.2.2.2`) for both of its interfaces, FRR can become confused during the master/slave negotiation process. Setting a high priority on one interface and a low priority on the other helps FRR to deterministically choose a master, preventing the adjacency from getting stuck in the `ExStart` state.

## 5. Timing and Latency Compensation

Analysis of packet captures revealed an asymmetric latency pattern between the DPDK simulator and the FRR router, primarily due to kernel processing delays on the FRR side. To ensure a stable OSPF adjacency, the simulator employs two key strategies:

### 1. Preemptive Hello Sending

*   **Problem**: The ~1ms kernel delay on the FRR side can cause timer drift, making it seem as though the simulator's Hello packets are arriving late. Over time, this can lead to the OSPF dead timer expiring.
*   **Solution**: The simulator sends Hello packets at 90% of the configured `hello_interval`. With a 2-second interval, this means sending a Hello every 1.8 seconds. This preemptive sending creates a buffer that absorbs the network and kernel latency, ensuring that FRR always receives a Hello packet well within the expected window.

### 2. Timeout Grace Period

*   **Problem**: A sudden spike in latency could still cause a Hello packet to arrive just after the dead timer is scheduled to expire.
*   **Solution**: The simulator adds a 500ms grace period to its dead timer calculation. With a 10-second dead interval, the simulator will not declare a neighbor dead until it has been silent for 10.5 seconds. This makes the simulator more tolerant of intermittent packet delays.

## 6. Known Deviations from RFC 2328

This implementation contains specific behaviors that deviate from the OSPFv2 standard. These changes were implemented to meet the explicit requirements of the target simulation environment.

### 1. Destination Address for All OSPF Packets

*   **RFC Standard**: On Point-to-Point networks, only Hello packets are sent to the multicast address `224.0.0.5`. After the `Init` state, all subsequent OSPF packets (DD, LSR, LSU, LSAck) should be sent via **unicast**.
*   **Simulator Implementation**: In this simulator, **all** OSPF packets are sent to the **multicast** address `224.0.0.5`.
    *   **Reason**: This was explicitly required for interoperability with the target environment.

### 2. Interface MTU in Database Description (DD) Packets

*   **RFC Standard**: For Point-to-Point interfaces, the MTU field in DD packets should be set to **0**.
*   **Simulator Implementation**: The MTU field is set to **0** in compliance with the RFC.
    *   **Reason**: This is a strict requirement for forming an adjacency with RFC-compliant routers like FRR.

### 3. Duplicate Router IDs

*   **RFC Standard**: Every router in an OSPF domain must have a unique Router ID.
*   **Simulator Implementation**: Both simulated router instances (PID 0 and PID 1) are configured with the same Router ID (`2.2.2.2`).
    *   **Reason**: This configuration was explicitly requested.
    *   **Warning**: Using duplicate Router IDs is a violation of the OSPF standard and can lead to unpredictable routing behavior and network instability. It is strongly recommended to use unique Router IDs in a production environment.

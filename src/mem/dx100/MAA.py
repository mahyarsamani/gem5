from m5.objects.ClockedObject import ClockedObject
from m5.params import *
from m5.proxy import *
from m5.SimObject import *

class MAA(ClockedObject):
    type = "MAA"
    cxx_header = "mem/dx100/MAA.hh"
    cxx_class = "gem5::MAA"

    num_tiles_per_core = Param.Unsigned(8, "Number of SPD tiles per core attached to the DX100 instance")
    num_tile_elements = Param.Unsigned(16384, "Number of elements in each tile")
    num_regs_per_core = Param.Unsigned(8, "Number of 32-bit scalar registers per core attached to the DX100 instance")
    num_instructions_per_core = Param.Unsigned(8, "Number of instructions in the instruction file per core attached to the DX100 instance")
    num_row_table_rows_per_slice = Param.Unsigned(64, "Number of rows in each row table slice")
    num_row_table_entries_per_subslice_row = Param.Unsigned(8, "Number of row table entries (bursts) per each sub-slice of row table")
    num_row_table_config_cache_entries = Param.Unsigned(16, "Number of row table entry history in the configuration cache")
    num_request_table_addresses = Param.Unsigned(128, "Number of addresses in the request table")
    num_request_table_entries_per_address = Param.Unsigned(16, "Number of entries in the request table per address")
    reconfigure_row_table = Param.Bool(False, "Reconfigure row table")
    no_reorder = Param.Bool(False, "Do not reorder accesses using row table")
    force_cache_access = Param.Bool(False, "Force cache access instead of direct memory access for the indirect access unit")
    num_initial_row_table_slices = Param.Unsigned(32, "Number of initial row table slices if row table is not reconfigurable")
    spd_read_latency = Param.Cycles(1, "SPD read latency")
    spd_write_latency = Param.Cycles(1, "SPD write latency")
    num_spd_read_ports_per_maa = Param.Unsigned(4, "Number of SPD read ports per DX100 instance")
    num_spd_write_ports_per_maa = Param.Unsigned(4, "Number of SPD write ports per DX100 instance")
    rowtable_latency = Param.Cycles(1, "Row table latency")
    ALU_lane_latency = Param.Cycles(1, "ALU lane latency")
    num_ALU_lanes = Param.Unsigned(16, "Number of ALU lanes")
    cache_snoop_latency = Param.Cycles(1, "Cache snoop latency")
    num_memory_channels = Param.Unsigned(2, "Number of memory channels")
    num_cores = Param.Unsigned(4, "Number of cores")
    num_maas = Param.Unsigned(1, "Number of MAA instances")

    # DX100 port to CHI: the fork's cpu_sides (a response port per core on
    # the memory bus), cache_sides (a request port per core into the LLC
    # bus) and mem_sides (a request port per DRAM channel around
    # coherence) become one register port and one data port per channel.
    # The fork's max_outstanding_cache_side_packets and
    # max_outstanding_cpu_side_packets (512) bounded those classic ports;
    # here the DMA requesters' TBEs and sequencers bound the data ports.
    pio_port = ResponsePort(
        "Register port: the cores' uncacheable accesses to every range but "
        "the cacheable scratchpad, from an IO crossbar"
    )
    data_ports = VectorRequestPort(
        "One port per DRAM channel, each into a CHI DMA requester's "
        "sequencer"
    )
    pio_latency = Param.Latency(
        "0ns",
        "Latency added to every register-port response, standing for the "
        "mesh round trip the IO crossbar bypasses",
    )

    addr_ranges = VectorParam.AddrRange(
        [AllMemory], "Address range for scratchpad data, scratchpad size, scratchpad ready, scalar registers, and instruction file"
    )
    region_range = Param.AddrRange(
        "Region registers (the fork's add_mem_region pseudo-op): start and "
        "end of region i at 16 i and 16 i + 8, a clear-all word after them"
    )
    dram = Param.DRAMInterface(
        "A DRAM interface of the memory channels, whose organisation and "
        "address map the Row Table and the channel routing follow (the "
        "fork's Ramulator2)"
    )
    mmu = Param.BaseMMU("MMU that translates the units' virtual addresses")

    system = Param.System(Parent.any, "System we belong to")

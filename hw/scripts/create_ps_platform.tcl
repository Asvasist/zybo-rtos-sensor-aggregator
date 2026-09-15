#------------------------------------------------------------------------------
# create_ps_platform.tcl
#
# Builds a PS-only Vivado project for the Zybo and exports the hardware
# handoff (.xsa) that the Vitis platform is created from.
#
# There is intentionally no PL logic here. The firmware stages only use PS
# peripherals (UART1, MIO GPIO, PS-XADC interface, TTC, SWDT), so the Zynq PS7
# block with the board preset applied is all Vitis needs. The proper PL design
# (AXI GPIO for SW0-3 / BTN0-3 etc.) comes after the firmware is finished.
#
# Usage, from the hw/ directory:
#   vivado -mode batch -source scripts/create_ps_platform.tcl -tclargs zybo-z7-20
#
# Board argument: zybo | zybo-z7-10 | zybo-z7-20     (default: zybo-z7-20)
# The Digilent board files have to be installed for the preset to apply.
#------------------------------------------------------------------------------

set board_name [expr {[llength $argv] > 0 ? [lindex $argv 0] : "zybo-z7-20"}]

set script_dir [file dirname [file normalize [info script]]]
set hw_dir     [file normalize [file join $script_dir ..]]
set proj_name  "zybo_ps_platform"
set proj_dir   [file join $hw_dir build $proj_name]
set bd_name    "ps_system"
set xsa_file   [file join $hw_dir export ${proj_name}.xsa]

# Newest installed revision of the board files for the requested board.
set board_part [lindex [lsort [get_board_parts -quiet -latest_file_version "*:${board_name}:*"]] end]
if {$board_part eq ""} {
    error "No board files found for '$board_name'. Install the Digilent board files and retry."
}
set part_name [get_property PART_NAME [get_board_parts $board_part]]
puts "INFO: board part $board_part, device $part_name"

create_project $proj_name $proj_dir -part $part_name -force
set_property board_part $board_part [current_project]

create_bd_design $bd_name

set ps7 [create_bd_cell -type ip -vlnv xilinx.com:ip:processing_system7:5.5 ps7]

# Board preset sets up DDR, MIO mux (UART1, GPIO on MIO7/50/51, ...) and clocks.
apply_bd_automation -rule xilinx.com:bd_rule:processing_system7 \
    -config {make_external "FIXED_IO, DDR" apply_board_preset "1" Master "Disable" Slave "Disable"} $ps7

# Nothing in the PL to talk to yet. Dropping GP0 avoids the validation error
# about an undriven M_AXI_GP0_ACLK, and the PL clock and reset the preset
# turns on have nothing to drive either.
#
# TTC0 is the sample timer from stage 2 on, the system watchdog (SWDT) is used
# from stage 4 on. Both are set explicitly, IO included - the GUI picks EMIO
# when you tick them, Tcl doesn't. Their outputs stay unconnected.
#
# MIO50/51 (BTN4/BTN5) have pull-down resistors on the board. The Zybo Z7
# preset leaves the Zynq's internal pull-ups on them enabled, which holds both
# buttons at "pressed"; the original Zybo preset already disables them. The
# firmware also clears these pull-ups at boot, so an XSA built without this
# still works - this just keeps the hardware description honest.
set_property -dict [list \
    CONFIG.PCW_USE_M_AXI_GP0          {0} \
    CONFIG.PCW_EN_CLK0_PORT           {0} \
    CONFIG.PCW_EN_RST0_PORT           {0} \
    CONFIG.PCW_TTC0_PERIPHERAL_ENABLE {1} \
    CONFIG.PCW_TTC0_TTC0_IO           {EMIO} \
    CONFIG.PCW_WDT_PERIPHERAL_ENABLE  {1} \
    CONFIG.PCW_WDT_WDT_IO             {EMIO} \
    CONFIG.PCW_MIO_50_PULLUP          {disabled} \
    CONFIG.PCW_MIO_51_PULLUP          {disabled} \
] $ps7

validate_bd_design
save_bd_design

set bd_file [get_files ${bd_name}.bd]
generate_target all $bd_file
add_files -norecurse [make_wrapper -files $bd_file -top]
set_property top ${bd_name}_wrapper [current_fileset]
update_compile_order -fileset sources_1

# Pre-synthesis export is enough: with an empty PL there is no bitstream to
# carry, ps7_init in the platform configures everything the firmware needs.
file mkdir [file dirname $xsa_file]
write_hw_platform -fixed -force -file $xsa_file

puts "INFO: hardware platform written to $xsa_file"

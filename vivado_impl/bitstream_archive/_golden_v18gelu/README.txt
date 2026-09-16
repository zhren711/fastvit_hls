GOLDEN ROLLBACK IMAGE -- identified by md5, NOT by file name
==============================================================

  fastvit_bd_wrapper_golden_swapped.bin   md5 7ee26f67a1fca38a2752e99cf0bac25b  (4,045,564 bytes)
      = the board's /lib/firmware/fastvit_bd_wrapper.bin, the load-ready (word-swapped) form.
  fastvit_bd_wrapper_golden.bit           md5 18173c41033816cb53ce0dd65ec1a64a  (raw Vivado .bit;
      bit_to_bin() of it gives exactly the 7ee26f67 .bin above)
  fastvit_bd_golden.hwh                   md5 78d20d0e4f193c1455df5f97eec2cba4

What it is: the pre-A3 "fastvit_ip" unified v18gelu bitstream (the 1,633.8ms design CLAUDE.md's
goal statement refers to; its ARM driver is fastvit_infer_v18gelu / fastvit_driver.c, a DIFFERENT
register map from every mac_array_a3_* bitstream -- it is a proof-of-life and rollback image only,
never a test target for the current harnesses).

Where copies live (2026-09-17, after the incident below):
  1. this directory (committed to git -- the .bit force-added past the *.bit ignore rule)
  2. build server patrick@192.168.1.87:~/fastvit_golden/  (both files, md5-named)
  3. board /home/root/fpga_unified_v18gelu/{fastvit_bd_wrapper.bit,.bin,.bin.bak}
  4. board /lib/firmware/fastvit_bd_wrapper.bin  (the live one)
Until 2026-09-17 ONLY copies 3 and 4 existed, and copy 4 was overwritten during a recovery.

NAME COLLISION -- the direct cause of that overwrite:
  /home/root/fpga/fastvit_bd_wrapper.bit and petalinux/hardware/fastvit_bd_wrapper.bit are NOT the
  golden image. They are the PetaLinux-era conv_ip/pool_ip bitstream (200MHz, the .xsa the board's
  Linux image was built against; bit_to_bin() gives md5 9f1b98a9...). Overlay(<that .bit>) converts
  it and writes /lib/firmware/fastvit_bd_wrapper.bin -- i.e. it silently replaces the golden .bin
  with a different design. The board copies were renamed on 2026-09-17 to
  fastvit_bd_wrapper_PETALINUX_convip_NOT_GOLDEN_9f1b98a9.{bit,bin} (test_overlay.py updated).

Proof-of-life procedure (loads the EXISTING .bin, converts nothing, overwrites nothing):
  md5sum /lib/firmware/fastvit_bd_wrapper.bin            # must be 7ee26f67...
  echo fastvit_bd_wrapper.bin > /sys/class/fpga_manager/fpga0/firmware
  cat /sys/class/fpga_manager/fpga0/state                 # operating
  md5sum /lib/firmware/fastvit_bd_wrapper.bin            # still 7ee26f67...
Restore, if the live copy is ever wrong again:
  scp <this dir>/fastvit_bd_wrapper_golden_swapped.bin root@192.168.1.50:/lib/firmware/fastvit_bd_wrapper.bin

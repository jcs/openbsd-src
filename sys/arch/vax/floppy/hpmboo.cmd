!
! BOOTSTRAP BY BOOT() SYSCALL
! GO MULTI-USER AFTER CHECKING; BOOT FROM DEFAULT DEVICE
!
SET DEF HEX
SET DEF LONG
SET REL:0
HALT
UNJAM
INIT
LOAD BOOT
D/G 8 0		! MEMORY IS NOT COUNTED IN BOOT
D/G A 0		! DEV TO BOOT FROM, SEE CODING IN REBOOT.H AND RPB.H
D/G B 0		! BOOT PARAMETERS: MULTI USER AFTER CHECK
D/G C FFFFFFFF	! SHOW THAT WE ARE NOT NETBOOTED
START 22

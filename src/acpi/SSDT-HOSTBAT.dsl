/*
 * Host laptop battery + AC adapter for the guest. Values come from the
 * QEMU "host-battery" device (build/qemu-wake hw/misc/host-battery.c) at
 * I/O 0x530, which mirrors /sys/class/power_supply/BAT0 + ADP1.
 * SMCBatteryManager.kext turns these into AppleSmartBattery.
 * Both devices report not-present when the QEMU device is missing.
 */
DefinitionBlock ("", "SSDT", 2, "HKVM", "HOSTBAT", 0x00001000)
{
    Scope (\_SB)
    {
        OperationRegion (HBRG, SystemIO, 0x0530, 0x40)
        Field (HBRG, DWordAcc, NoLock, Preserve)
        {
            HMAG, 32, HSTA, 32, HRAT, 32, HREM, 32,
            HVOL, 32, HDCA, 32, HLFC, 32, HDVO, 32,
            HACO, 32, HCYC, 32, HUNI, 32, HPRS, 32,
            HPCT, 32
        }

        Method (HBOK, 0, NotSerialized)
        {
            Return (LEqual (HMAG, 0x54414248))
        }

        Device (HADP)
        {
            Name (_HID, "ACPI0003")
            Name (_PCL, Package () { \_SB })
            Method (_STA, 0, NotSerialized)
            {
                If (HBOK ()) { Return (0x0F) }
                Return (Zero)
            }
            Method (_PSR, 0, NotSerialized)
            {
                Return (HACO)
            }
        }

        Device (HBAT)
        {
            Name (_HID, EisaId ("PNP0C0A"))
            Name (_UID, One)
            Name (_PCL, Package () { \_SB })
            Method (_STA, 0, NotSerialized)
            {
                If (HBOK ())
                {
                    If (HPRS) { Return (0x1F) }
                    Return (0x0F)
                }
                Return (Zero)
            }
            Method (_BIF, 0, Serialized)
            {
                Name (BIFP, Package (0x0D)
                {
                    One, 0xFFFFFFFF, 0xFFFFFFFF, One, 0xFFFFFFFF,
                    Zero, Zero, One, One,
                    "Laptop Battery", "HKVM0001", "LION", "Host"
                })
                Store (HUNI, Index (BIFP, Zero))
                Store (HDCA, Index (BIFP, One))
                Store (HLFC, Local0)
                Store (Local0, Index (BIFP, 0x02))
                Store (HDVO, Index (BIFP, 0x04))
                Store (Divide (Local0, 10), Index (BIFP, 0x05))
                Store (Divide (Local0, 20), Index (BIFP, 0x06))
                Return (BIFP)
            }
            Method (_BST, 0, Serialized)
            {
                Name (BSTP, Package (0x04) { Zero, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF })
                Store (HSTA, Index (BSTP, Zero))
                Store (HRAT, Index (BSTP, One))
                Store (HREM, Index (BSTP, 0x02))
                Store (HVOL, Index (BSTP, 0x03))
                Return (BSTP)
            }
        }
    }
}

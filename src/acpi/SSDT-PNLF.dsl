/*
 * Backlight device for the internal eDP panel. IOFramebuffer only builds an
 * AppleBacklightDisplay (brightness slider/keys) when an IORegistry entry
 * named "backlight" exists; the AMD framebuffer then drives the PWM itself.
 */
DefinitionBlock ("", "SSDT", 2, "HKVM", "PNLF", 0x00001000)
{
    Scope (\_SB)
    {
        Device (PNLF)
        {
            Name (_HID, EisaId ("APP0002"))
            Name (_CID, "backlight")
            Name (_UID, 0x10)
            Method (_STA, 0, NotSerialized)
            {
                Return (0x0B)
            }
        }
    }
}

// RmbGop: minimal UEFI GOP for the passed-through 680M (Rembrandt, DCN 3.1.2).
// The 680M has no working UEFI GOP inside the VM, but the host firmware GOP leaves OTG0/HUBP0
// scanning a linear framebuffer on the internal eDP panel. If that scanout is still running at
// VM start, expose it (through BAR0) as a GOP and make it the console GOP, so OpenCore/boot.efi
// and the macOS boot progress UI draw on the internal panel. Does nothing if the scanout is not
// usable (OTG off, tiled surface, odd format, outside BAR0) -> the QEMU display stays the console.
// Freestanding (no EDK2): build with src/RmbGop/Makefile. Log lines go to COM1 (logs/ovmf-serial.log).

typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef unsigned long long u64;
typedef u64 uintn;
typedef u64 status_t;
typedef void *handle_t;
typedef u16 char16;

#define EFIAPI __attribute__((ms_abi))
#define EFI_ERR(s) ((s) >> 63)
#define EFI_UNSUPPORTED 0x8000000000000003ULL
#define EFI_INVALID_PARAMETER 0x8000000000000002ULL

typedef struct { u32 a; u16 b, c; u8 d[8]; } guid_t;
static const guid_t GopGuid = {0x9042a9de, 0x23dc, 0x4a38, {0x96, 0xfb, 0x7a, 0xde, 0xd0, 0x80, 0x51, 0x6a}};
static const guid_t PciIoGuid = {0x4cf5b200, 0x68b8, 0x4ca5, {0x9e, 0xec, 0xb2, 0x3e, 0x3f, 0x50, 0x02, 0x9a}};

typedef struct { u64 sig; u32 rev, size, crc, rsvd; } table_hdr;

typedef struct text_out {
    status_t (EFIAPI *Reset)(struct text_out *, u8);
    status_t (EFIAPI *OutputString)(struct text_out *, const char16 *);
} text_out;

typedef struct {
    table_hdr Hdr;
    void *RaiseTPL, *RestoreTPL;
    void *AllocatePages, *FreePages, *GetMemoryMap;
    status_t (EFIAPI *AllocatePool)(u32 type, uintn size, void **buf);
    status_t (EFIAPI *FreePool)(void *buf);
    status_t (EFIAPI *CreateEvent)(u32 type, uintn tpl, void (EFIAPI *fn)(void *, void *), void *ctx, void **ev);
    void *SetTimer, *WaitForEvent, *SignalEvent, *CloseEvent, *CheckEvent;
    status_t (EFIAPI *InstallProtocolInterface)(handle_t *h, const guid_t *g, u32 type, void *iface);
    void *ReinstallProtocolInterface;
    status_t (EFIAPI *UninstallProtocolInterface)(handle_t h, const guid_t *g, void *iface);
    status_t (EFIAPI *HandleProtocol)(handle_t h, const guid_t *g, void **iface);
    void *Reserved, *RegisterProtocolNotify, *LocateHandle, *LocateDevicePath, *InstallConfigurationTable;
    void *LoadImage, *StartImage, *Exit, *UnloadImage, *ExitBootServices;
    void *GetNextMonotonicCount;
    status_t (EFIAPI *Stall)(uintn us);
    void *SetWatchdogTimer;
    void *ConnectController, *DisconnectController;
    void *OpenProtocol, *CloseProtocol, *OpenProtocolInformation;
    void *ProtocolsPerHandle;
    status_t (EFIAPI *LocateHandleBuffer)(u32 type, const guid_t *g, void *key, uintn *n, handle_t **buf);
    void *LocateProtocol, *InstallMultipleProtocolInterfaces, *UninstallMultipleProtocolInterfaces;
    void *CalculateCrc32, *CopyMem, *SetMem, *CreateEventEx;
} boot_services;

typedef struct {
    table_hdr Hdr;
    char16 *FirmwareVendor;
    u32 FirmwareRevision;
    handle_t ConsoleInHandle;
    void *ConIn;
    handle_t ConsoleOutHandle;
    text_out *ConOut;
    handle_t StandardErrorHandle;
    text_out *StdErr;
    void *RuntimeServices;
    boot_services *BootServices;
} system_table;

typedef struct pci_io pci_io;
typedef status_t (EFIAPI *pci_mem_fn)(pci_io *, u32 width, u8 bar, u64 off, uintn count, void *buf);
typedef status_t (EFIAPI *pci_cfg_fn)(pci_io *, u32 width, u32 off, uintn count, void *buf);
struct pci_io {
    void *PollMem, *PollIo;
    struct { pci_mem_fn Read, Write; } Mem;
    struct { void *Read, *Write; } Io;
    struct { pci_cfg_fn Read, Write; } Pci;
    void *CopyMem, *Map, *Unmap, *AllocateBuffer, *FreeBuffer, *Flush;
    status_t (EFIAPI *GetLocation)(pci_io *, uintn *seg, uintn *bus, uintn *dev, uintn *fn);
    status_t (EFIAPI *Attributes)(pci_io *, u32 op, u64 attrs, u64 *result);
};
enum { PciWidthUint32 = 2, PciAttrSupported = 4, PciAttrEnable = 2 };
#define PCI_DEVICE_ENABLE 0x0700ULL  // IO | MEMORY | BUS_MASTER

typedef struct {
    u32 Version, HorizontalResolution, VerticalResolution;
    u32 PixelFormat;  // 0 = RGBX8888 bytes, 1 = BGRX8888 bytes
    u32 PixelInformation[4];
    u32 PixelsPerScanLine;
} gop_mode_info;

typedef struct {
    u32 MaxMode, Mode;
    gop_mode_info *Info;
    uintn SizeOfInfo;
    u64 FrameBufferBase;
    uintn FrameBufferSize;
} gop_mode;

typedef struct gop gop;
struct gop {
    status_t (EFIAPI *QueryMode)(gop *, u32 mode, uintn *size, gop_mode_info **info);
    status_t (EFIAPI *SetMode)(gop *, u32 mode);
    status_t (EFIAPI *Blt)(gop *, u32 *buf, u32 op, uintn sx, uintn sy, uintn dx, uintn dy, uintn w, uintn h, uintn delta);
    gop_mode *Mode;
};

// clang may emit calls to these even freestanding
void *memcpy(void *d, const void *s, uintn n) { u8 *dd = d; const u8 *ss = s; while (n--) *dd++ = *ss++; return d; }
void *memset(void *d, int c, uintn n) { u8 *dd = d; while (n--) *dd++ = (u8)c; return d; }

static system_table *ST;
static boot_services *BS;
static gop_mode_info Info;
static gop_mode Mode;
static gop Gop;
static volatile u32 *Fb;
static int SwapRB;  // surface is ABGR8888 (RGBX bytes); Blt buffers are always BGRX

// ---- logging ----
// Straight to COM1 (QEMU -serial file:logs/ovmf-serial.log): OpenCore replaces ConOut with its
// own graphics text renderer, so ConOut text would not reach the serial log.
static inline u8 inb(u16 port) { u8 v; __asm__ volatile("inb %1, %0" : "=a"(v) : "Nd"(port)); return v; }
static inline void outb(u16 port, u8 v) { __asm__ volatile("outb %0, %1" : : "a"(v), "Nd"(port)); }
static void serial_putc(char c)
{
    for (int i = 0; i < 100000 && !(inb(0x3fd) & 0x20); i++) {}
    outb(0x3f8, (u8)c);
}
static void puts16(const char *s)
{
    for (; *s; s++) {
        if (*s == '\n') serial_putc('\r');
        serial_putc(*s);
    }
}

static void log_hex(const char *msg, u64 v)
{
    char b[96];
    int i = 0;
    for (const char *p = "RmbGop: "; *p; p++) b[i++] = *p;
    for (; *msg && i < 70; msg++) b[i++] = *msg;
    b[i++] = ' ';
    b[i++] = '0';
    b[i++] = 'x';
    int started = 0;
    for (int sh = 60; sh >= 0; sh -= 4) {
        int d = (v >> sh) & 0xf;
        if (d || started || sh == 0) {
            b[i++] = "0123456789abcdef"[d];
            started = 1;
        }
    }
    b[i++] = '\n';
    b[i] = 0;
    puts16(b);
}

// ---- GOP ----
static inline u32 swap_rb(u32 p)
{
    return SwapRB ? ((p & 0xff00ff00) | ((p & 0xff) << 16) | ((p >> 16) & 0xff)) : p;
}

static status_t EFIAPI query_mode(gop *g, u32 mode, uintn *size, gop_mode_info **info)
{
    (void)g;
    if (mode != 0 || !size || !info) return EFI_INVALID_PARAMETER;
    void *p;
    status_t s = BS->AllocatePool(4 /* EfiBootServicesData */, sizeof Info, &p);
    if (EFI_ERR(s)) return s;
    __builtin_memcpy(p, &Info, sizeof Info);
    *info = p;
    *size = sizeof Info;
    return 0;
}

static status_t EFIAPI set_mode(gop *g, u32 mode)
{
    (void)g;
    return mode == 0 ? 0 : EFI_UNSUPPORTED;
}

static status_t EFIAPI blt(gop *g, u32 *buf, u32 op, uintn sx, uintn sy, uintn dx, uintn dy,
                           uintn w, uintn h, uintn delta)
{
    (void)g;
    const uintn W = Info.HorizontalResolution, H = Info.VerticalResolution, P = Info.PixelsPerScanLine;
    if (!w || !h) return EFI_INVALID_PARAMETER;
    if (!delta) delta = w * 4;
    const uintn bstride = delta / 4;
    switch (op) {
    case 0:  // VideoFill: buf[0] -> video
        if (dx + w > W || dy + h > H) return EFI_INVALID_PARAMETER;
        for (uintn y = 0; y < h; y++) {
            volatile u32 *d = Fb + (dy + y) * P + dx;
            const u32 px = swap_rb(buf[0]);
            for (uintn x = 0; x < w; x++) d[x] = px;
        }
        return 0;
    case 1:  // VideoToBltBuffer
        if (sx + w > W || sy + h > H) return EFI_INVALID_PARAMETER;
        for (uintn y = 0; y < h; y++) {
            volatile u32 *s = Fb + (sy + y) * P + sx;
            u32 *d = buf + (dy + y) * bstride + dx;
            for (uintn x = 0; x < w; x++) d[x] = swap_rb(s[x]);
        }
        return 0;
    case 2:  // BufferToVideo
        if (dx + w > W || dy + h > H) return EFI_INVALID_PARAMETER;
        for (uintn y = 0; y < h; y++) {
            const u32 *s = buf + (sy + y) * bstride + sx;
            volatile u32 *d = Fb + (dy + y) * P + dx;
            for (uintn x = 0; x < w; x++) d[x] = swap_rb(s[x]);
        }
        return 0;
    case 3:  // VideoToVideo (overlap-safe by row order)
        if (sx + w > W || sy + h > H || dx + w > W || dy + h > H) return EFI_INVALID_PARAMETER;
        if (dy <= sy) {
            for (uintn y = 0; y < h; y++) {
                volatile u32 *s = Fb + (sy + y) * P + sx, *d = Fb + (dy + y) * P + dx;
                if (dx <= sx) for (uintn x = 0; x < w; x++) d[x] = s[x];
                else for (uintn x = w; x-- > 0;) d[x] = s[x];
            }
        } else {
            for (uintn y = h; y-- > 0;) {
                volatile u32 *s = Fb + (sy + y) * P + sx, *d = Fb + (dy + y) * P + dx;
                if (dx <= sx) for (uintn x = 0; x < w; x++) d[x] = s[x];
                else for (uintn x = w; x-- > 0;) d[x] = s[x];
            }
        }
        return 0;
    }
    return EFI_INVALID_PARAMETER;
}

// ---- 680M detection ----
// DCN 3.1.2 register dword offsets (BAR5), as printed by scripts/dcreg.py
enum {
    DCN_VM_FB_LOCATION_BASE = 0x3935,
    HUBP0_DCSURF_SURFACE_CONFIG = 0x3aa5,
    HUBP0_DCSURF_TILING_CONFIG = 0x3aa7,
    HUBP0_DCSURF_PRI_VIEWPORT_DIMENSION = 0x3aaa,
    HUBPREQ0_DCSURF_SURFACE_PITCH = 0x3ac7,
    HUBPREQ0_DCSURF_PRIMARY_SURFACE_ADDRESS = 0x3aca,
    HUBPREQ0_DCSURF_PRIMARY_SURFACE_ADDRESS_HIGH = 0x3acb,
    HUBPREQ0_DCSURF_SURFACE_CONTROL = 0x3ada,
    OTG0_OTG_CONTROL = 0x5001,
};

// BAR5 register access: through PciIo, or (if PciIo reads fail) directly at the BAR5 address
// from config space (OVMF identity-maps MMIO).
static volatile u32 *Mmio;
static u32 rreg(pci_io *io, u32 dw)
{
    if (Mmio) return Mmio[dw];
    u32 v = 0xffffffff;
    io->Mem.Read(io, PciWidthUint32, 5, (u64)dw * 4, 1, &v);
    return v;
}

// VBIOSSMC message (as the kext's vbiosSmcSend / Linux dcn31_smu_send_msg_with_param), direct MMIO only.
static void spin(u32 n)
{
    for (volatile u32 i = 0; i < n; i++)
        ;
}
static u32 vbios_smc(u32 msg, u32 param)
{
    enum { Msg = 0x16283, Arg = 0x16293, Resp = 0x1629b };
    for (u32 t = 0; t < 1000 && Mmio[Resp] == 0; t++) spin(10000);
    Mmio[Resp] = 0;
    Mmio[Arg] = param;
    Mmio[Msg] = msg;
    u32 r = 0;
    for (u32 t = 0; t < 20000 && (r = Mmio[Resp]) == 0; t++) spin(10000);
    return r;
}

// Find a PCI capability, 0 if absent
static u32 find_cap(pci_io *io, u8 id)
{
    u32 r;
    io->Pci.Read(io, PciWidthUint32, 0x34, 1, &r);
    u32 p = r & 0xfc;
    for (int i = 0; p && i < 48; i++) {
        io->Pci.Read(io, PciWidthUint32, p, 1, &r);
        if ((r & 0xff) == id) return p;
        p = (r >> 8) & 0xfc;
    }
    return 0;
}

// rmbgop1: every PciIo BAR5 read was 0xffffffff even with memory decode on. Log why, and
// pick a working access path (PciIo, direct MMIO, after forcing D0, after waiting).
static int setup_mmio(pci_io *io)
{
    u32 bar5 = 0;
    io->Pci.Read(io, PciWidthUint32, 0x24, 1, &bar5);
    log_hex("BAR5 config", bar5);
    const u32 pm = find_cap(io, 0x01);
    u32 pmcsr = 0;
    if (pm) io->Pci.Read(io, PciWidthUint32, pm + 4, 1, &pmcsr);
    log_hex("PM cap", pm);
    log_hex("PMCSR", pmcsr);
    if (pm && (pmcsr & 3)) {
        u32 d0 = (pmcsr & ~3u) & ~0x8000u;  // D0, don't clear PME status
        io->Pci.Write(io, PciWidthUint32, pm + 4, 1, &d0);
        BS->Stall(20000);
        io->Pci.Read(io, PciWidthUint32, pm + 4, 1, &pmcsr);
        log_hex("PMCSR after D0", pmcsr);
    }
    for (int t = 0; t < 50; t++) {
        u32 v = 0xffffffff;
        const status_t s = io->Mem.Read(io, PciWidthUint32, 5, (u64)OTG0_OTG_CONTROL * 4, 1, &v);
        const u32 direct = (bar5 & ~0xfu) ? ((volatile u32 *)(uintn)(bar5 & ~0xfu))[OTG0_OTG_CONTROL] : 0xffffffff;
        if (t == 0 || v != 0xffffffff || direct != 0xffffffff) {
            log_hex("try", t);
            log_hex("PciIo Mem.Read status", s);
            log_hex("PciIo OTG0_OTG_CONTROL", v);
            log_hex("direct OTG0_OTG_CONTROL", direct);
        }
        if (!EFI_ERR(s) && v != 0xffffffff) return 1;
        if (direct != 0xffffffff) {
            Mmio = (volatile u32 *)(uintn)(bar5 & ~0xfu);
            puts16("RmbGop: using direct BAR5 MMIO\n");
            return 1;
        }
        BS->Stall(100000);
    }
    puts16("RmbGop: BAR5 still all-ones after 5 s\n");
    u32 cmd = 0, id = 0;
    io->Pci.Read(io, PciWidthUint32, 4, 1, &cmd);
    io->Pci.Read(io, PciWidthUint32, 0, 1, &id);
    log_hex("PCI command/status", cmd);
    log_hex("PCI id", id);
    return 0;
}

static int probe(pci_io *io)
{
    u32 id;
    if (EFI_ERR(io->Pci.Read(io, PciWidthUint32, 0, 1, &id)) || (id & 0xffff) != 0x1002) return 0;
    u32 cls;
    io->Pci.Read(io, PciWidthUint32, 8, 1, &cls);
    if ((cls >> 24) != 0x03) return 0;
    log_hex("found AMD display device, id", id);

    u64 sup = 0;
    io->Attributes(io, PciAttrSupported, 0, &sup);
    const status_t es = io->Attributes(io, PciAttrEnable, sup & PCI_DEVICE_ENABLE, 0);
    u32 cmd = 0;
    io->Pci.Read(io, PciWidthUint32, 4, 1, &cmd);
    log_hex("attributes supported", sup);
    log_hex("enable status", es);
    log_hex("PCI command", cmd & 0xffff);

    u32 bar0[2];
    io->Pci.Read(io, PciWidthUint32, 0x10, 2, bar0);
    const u64 bar0base = ((u64)bar0[1] << 32) | (bar0[0] & ~0xfu);
    const u64 bar0size = 0x10000000;  // 256 MB aperture over the UMA carve-out (QEMU info pci)

    if (!setup_mmio(io)) return 0;
    {
        // BAR0 sanity: first framebuffer dword through PciIo
        u32 v0 = 0xdeadbeef;
        const status_t s0 = io->Mem.Read(io, PciWidthUint32, 0, 0, 1, &v0);
        log_hex("BAR0[0] status", s0);
        log_hex("BAR0[0]", v0);
    }
    const u32 otg = rreg(io, OTG0_OTG_CONTROL);
    const u32 tiling = rreg(io, HUBP0_DCSURF_TILING_CONFIG);
    const u32 fmt = rreg(io, HUBP0_DCSURF_SURFACE_CONFIG) & 0x7f;
    const u32 vp = rreg(io, HUBP0_DCSURF_PRI_VIEWPORT_DIMENSION);
    const u32 pitch = (rreg(io, HUBPREQ0_DCSURF_SURFACE_PITCH) & 0x3fff) + 1;
    const u32 sctl = rreg(io, HUBPREQ0_DCSURF_SURFACE_CONTROL);
    const u64 addr = ((u64)(rreg(io, HUBPREQ0_DCSURF_PRIMARY_SURFACE_ADDRESS_HIGH) & 0xffff) << 32) |
                     rreg(io, HUBPREQ0_DCSURF_PRIMARY_SURFACE_ADDRESS);
    const u64 fbbase = (u64)(rreg(io, DCN_VM_FB_LOCATION_BASE) & 0xffffff) << 24;
    const u32 w = vp & 0x3fff, h = (vp >> 16) & 0x3fff;
    log_hex("BAR0", bar0base);
    log_hex("BAR0 size", bar0size);
    log_hex("OTG0_OTG_CONTROL", otg);
    log_hex("HUBP0 tiling", tiling);
    log_hex("HUBP0 format", fmt);
    log_hex("HUBP0 viewport", vp);
    log_hex("HUBP0 pitch", pitch);
    log_hex("HUBP0 surface control", sctl);
    log_hex("HUBP0 surface", addr);
    log_hex("FB location base", fbbase);

    if (!(otg & 1)) { puts16("RmbGop: OTG0 not running, no GOP\n"); return 0; }
    if ((tiling & 0x1f) != 0) { puts16("RmbGop: surface not linear, no GOP\n"); return 0; }
    if (fmt != 8 && fmt != 10) { puts16("RmbGop: unsupported pixel format, no GOP\n"); return 0; }
    if (!w || !h || pitch < w) { puts16("RmbGop: bad geometry, no GOP\n"); return 0; }
    const u64 off = addr - fbbase;
    const u64 size = (u64)pitch * h * 4;
    if (addr < fbbase || off + size > bar0size) { puts16("RmbGop: surface outside BAR0, no GOP\n"); return 0; }

    SwapRB = fmt == 10;
    Info.Version = 0;
    Info.HorizontalResolution = w;
    Info.VerticalResolution = h;
    Info.PixelFormat = SwapRB ? 0 : 1;
    Info.PixelsPerScanLine = pitch;
    Mode.MaxMode = 1;
    Mode.Mode = 0;
    Mode.Info = &Info;
    Mode.SizeOfInfo = sizeof Info;
    Mode.FrameBufferBase = bar0base + off;
    Mode.FrameBufferSize = size;
    Fb = (volatile u32 *)Mode.FrameBufferBase;
    Gop.QueryMode = query_mode;
    Gop.SetMode = set_mode;
    Gop.Blt = blt;
    Gop.Mode = &Mode;
    log_hex("GOP framebuffer", Mode.FrameBufferBase);
    return 1;
}

// ---- mirror mode (default) ----
// Console-GOP takeover moved the macOS boot console to the 680M: IONDRVFramebuffer then has no
// usable fb at WindowServer start, WS never builds a display pipe for the AMD fb (blank login,
// needed a WS restart) and the restarted WS breaks sleep (PPLIB resume panic, auto-1017-1727).
// Instead keep the QEMU display as the console and mirror what is drawn on it to the panel:
// wrap its Blt/SetMode, and copy the whole screen once more at ExitBootServices (boot.efi's last
// frame, i.e. the Apple logo, stays on the panel until the kext's modeset).
// Mirror mode FAILED (auto-1017-1759): svga NDRV fb at 1280x800 had no WS display pipe, panel never came up.
#ifndef RMBGOP_CONSOLE
#define RMBGOP_CONSOLE 1
#endif
static gop *Svga;
static status_t (EFIAPI *SvgaBlt)(gop *, u32 *, u32, uintn, uintn, uintn, uintn, uintn, uintn, uintn);
static status_t (EFIAPI *SvgaSetMode)(gop *, u32);
static int Mirroring;

// Copy svga rect (x, y, w, h) to the panel, scaled to fit (nearest) and centered.
static void mirror_rect(uintn x, uintn y, uintn w, uintn h)
{
    if (!Mirroring) return;
    const gop_mode *m = Svga->Mode;
    const uintn SW = m->Info->HorizontalResolution, SH = m->Info->VerticalResolution, SP = m->Info->PixelsPerScanLine;
    const uintn PW = Info.HorizontalResolution, PH = Info.VerticalResolution, PP = Info.PixelsPerScanLine;
    if (!SW || !SH || x >= SW || y >= SH) return;
    if (x + w > SW) w = SW - x;
    if (y + h > SH) h = SH - y;
    u64 sc = ((u64)PW << 16) / SW;  // 16.16 scale = min(PW/SW, PH/SH)
    if ((((u64)PH << 16) / SH) < sc) sc = ((u64)PH << 16) / SH;
    const uintn ox = (PW - ((SW * sc) >> 16)) / 2, oy = (PH - ((SH * sc) >> 16)) / 2;
    const uintn px0 = ox + ((x * sc) >> 16), px1 = ox + (((x + w) * sc + 0xffff) >> 16);
    const uintn py0 = oy + ((y * sc) >> 16), py1 = oy + (((y + h) * sc + 0xffff) >> 16);
    const volatile u32 *src = (const volatile u32 *)m->FrameBufferBase;
    const int srgb = m->Info->PixelFormat == 0;  // svga RGBX -> BGRX first
    for (uintn py = py0; py < py1 && py < PH; py++) {
        const uintn sy = ((py - oy) << 16) / sc;
        if (sy >= SH) break;
        volatile u32 *d = Fb + py * PP;
        for (uintn px = px0; px < px1 && px < PW; px++) {
            const uintn sx = ((px - ox) << 16) / sc;
            if (sx >= SW) break;
            u32 v = src[sy * SP + sx];
            if (srgb) v = (v & 0xff00ff00) | ((v & 0xff) << 16) | ((v >> 16) & 0xff);
            d[px] = swap_rb(v);
        }
    }
}

static status_t EFIAPI mirror_blt(gop *g, u32 *buf, u32 op, uintn sx, uintn sy, uintn dx, uintn dy,
                                  uintn w, uintn h, uintn delta)
{
    const status_t s = SvgaBlt(g, buf, op, sx, sy, dx, dy, w, h, delta);
    if (!EFI_ERR(s) && op != 1) mirror_rect(dx, dy, w, h);  // every op but VideoToBltBuffer writes video
    return s;
}

static status_t EFIAPI mirror_set_mode(gop *g, u32 mode)
{
    const status_t s = SvgaSetMode(g, mode);
    if (!EFI_ERR(s)) {
        u32 black = 0;
        blt(&Gop, &black, 0, 0, 0, 0, 0, Info.HorizontalResolution, Info.VerticalResolution, 0);
        mirror_rect(0, 0, Svga->Mode->Info->HorizontalResolution, Svga->Mode->Info->VerticalResolution);
    }
    return s;
}

// ExitBootServices: last full copy (boot.efi may also draw straight into the fb), then stop.
static void EFIAPI mirror_exit_bs(void *ev, void *ctx)
{
    (void)ev; (void)ctx;
    mirror_rect(0, 0, Svga->Mode->Info->HorizontalResolution, Svga->Mode->Info->VerticalResolution);
    Mirroring = 0;
}

static int start_mirror(void)
{
    if (EFI_ERR(BS->HandleProtocol(ST->ConsoleOutHandle, &GopGuid, (void **)&Svga)) || !Svga || !Svga->Mode ||
        !Svga->Mode->Info) {
        puts16("RmbGop: no ConOut GOP to mirror\n");
        return 0;
    }
    log_hex("mirroring ConOut GOP, width", Svga->Mode->Info->HorizontalResolution);
    log_hex("mirroring ConOut GOP, height", Svga->Mode->Info->VerticalResolution);
    SvgaBlt = Svga->Blt;
    SvgaSetMode = Svga->SetMode;
    Svga->Blt = mirror_blt;
    Svga->SetMode = mirror_set_mode;
    Mirroring = 1;
    void *ev;
    log_hex("ExitBootServices event", BS->CreateEvent(0x201 /* EVT_SIGNAL_EXIT_BOOT_SERVICES */, 16 /* TPL_NOTIFY */,
                                                      mirror_exit_bs, 0, &ev));
    mirror_rect(0, 0, Svga->Mode->Info->HorizontalResolution, Svga->Mode->Info->VerticalResolution);
    return 1;
}

status_t EFIAPI efi_main(handle_t image, system_table *st)
{
    (void)image;
    ST = st;
    BS = st->BootServices;
    puts16("RmbGop: start\n");

    uintn n = 0;
    handle_t *hs = 0;
    if (EFI_ERR(BS->LocateHandleBuffer(2 /* ByProtocol */, &PciIoGuid, 0, &n, &hs))) return EFI_UNSUPPORTED;
    handle_t dev = 0;
    for (uintn i = 0; i < n && !dev; i++) {
        pci_io *io;
        if (!EFI_ERR(BS->HandleProtocol(hs[i], &PciIoGuid, (void **)&io)) && probe(io)) dev = hs[i];
    }
    BS->FreePool(hs);
    // No usable firmware scanout (no 680M, or a same-boot VM restart left macOS's tiled surface):
    // succeed without a GOP -- OpenCore halts on a driver that returns an error.
    if (!dev) return 0;

    // rmbgop4: logo intact in VRAM and scanout regs unchanged, yet the panel goes dark ~5 s later --
    // same timer-like async death as the PHY under DAL before the kext's VBIOSSMC Z-state fix.
    // Send that fix here too (VBIOSSMC mailbox MP1_SMN_C2PMSG_67/83/91).
    if (Mmio) {
        log_hex("RDPCSTX0_CNTL before VBIOSSMC", Mmio[0x5df0]);
        log_hex("DisallowZstatesEntry resp", vbios_smc(0x16, 0));
        log_hex("SetDisplayIdleOptimizations(0) resp", vbios_smc(0x12, 0));
        log_hex("RDPCSTX0_CNTL after VBIOSSMC", Mmio[0x5df0]);
        // Logo dies ~5 s in as colorful horizontal bands with fb + scanout regs intact: DRAM
        // self-refresh (stutter) on the GOP's watermarks once the host idles. Force SR disallowed
        // (DCHUBBUB_ARB_DRAM_STATE_CNTL FORCE_ENABLE=1, VALUE=0), as the kext does from its load.
        log_hex("DRAM_STATE_CNTL before", Mmio[0x39bc]);
        Mmio[0x39bc] = (Mmio[0x39bc] & ~1u) | 2u;
        log_hex("DRAM_STATE_CNTL after", Mmio[0x39bc]);
        // auto-1017-1727: SR was already disallowed and the logo still died when the macOS kernel
        // started, with HUBP still fetching -> suspect underflow from DRAM contention at the
        // firmware's minimum DCFCLK. Vote display clocks like the kext (dmub.cpp), and clear the
        // sticky HUBP0 underflow so the timeline shows whether it recurs at kernel start.
        log_hex("SetHardMinDcfclk(600) resp", vbios_smc(0x7, 600));
        log_hex("SetMinDeepSleepDcfclk(400) resp", vbios_smc(0x8, 400));
        log_hex("HUBP0_DCHUBP_CNTL before clear", Mmio[0x3ab3]);
        Mmio[0x3ab3] |= 1u << 31;   // HUBP_UNDERFLOW_CLEAR
        Mmio[0x3ab3] &= ~(1u << 31);
        log_hex("HUBP0_DCHUBP_CNTL after clear", Mmio[0x3ab3]);
    }

    // Clear to black before anyone draws (the panel still shows the host's last image).
    u32 black = 0;
    blt(&Gop, &black, 0, 0, 0, 0, 0, Info.HorizontalResolution, Info.VerticalResolution, 0);

    if (!RMBGOP_CONSOLE) {
        start_mirror();
        return 0;
    }

    // GOP on the PCI handle (device path PciRoot(0x0)/Pci(0x10,0x0)) ...
    status_t s = BS->InstallProtocolInterface(&dev, &GopGuid, 0, &Gop);
    log_hex("install GOP on 680M handle", s);
    // ... and make it the console GOP, replacing the QEMU display's one, so OpenCore
    // (ProvideConsoleGop keeps an existing ConOut GOP) and boot.efi draw on the panel.
    handle_t con = ST->ConsoleOutHandle;
    gop *old;
    if (!EFI_ERR(BS->HandleProtocol(con, &GopGuid, (void **)&old))) {
        s = BS->UninstallProtocolInterface(con, &GopGuid, old);
        log_hex("uninstall ConOut GOP", s);
    }
    s = BS->InstallProtocolInterface(&con, &GopGuid, 0, &Gop);
    log_hex("install GOP on ConOut", s);
    return 0;
}

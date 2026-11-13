#include "generic_provider.h"
#include "bounded_reader.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <set>

namespace cortex::artifacts {
namespace {
using bounded::View;

constexpr uint64_t kScanLimit = 64ull << 20;   // bytes read for text, strings and embedded files
constexpr uint64_t kEntropyLimit = 1ull << 30;  // bytes streamed for the entropy profile

#define B(literal) std::string(literal, sizeof(literal) - 1)

using Check = bool (*)(const View&, uint64_t size);

struct Signature {
    uint64_t offset;
    std::string bytes;
    const char* name;
    const char* category;
    const char* mime;
    Check check = nullptr;  // extra test for short or common magics
};

bool BmpCheck(const View& v, uint64_t size) {
    if (!v.Has(14, 4)) return false;
    const uint32_t header = v.U32(14), declared = v.U32(2);
    return (header == 12 || header == 40 || header == 52 || header == 56 || header == 108 || header == 124) && declared <= size + 16;
}
bool IcoCheck(const View& v, uint64_t) { return v.Has(4, 2) && v.U16(4) >= 1 && v.U16(4) <= 256 && v.Has(6, 16) && v.U8(9) == 0; }
bool TtfCheck(const View& v, uint64_t) { return v.Has(4, 2) && v.U16(4, false) >= 4 && v.U16(4, false) <= 64; }
bool FtypCheck(const View& v, uint64_t size) { const uint32_t box = v.U32(0, false); return box >= 8 && box <= 512 && box <= size; }
bool Ext2Check(const View& v, uint64_t) { return v.Has(1024 + 24, 4) && v.U32(1024 + 24) <= 6 && v.U32(1024 + 4) > 0; }
bool MbrCheck(const View& v, uint64_t) {
    if (!v.Has(446, 64)) return false;
    unsigned used = 0;
    for (unsigned i = 0; i < 4; ++i) {
        const uint8_t status = v.U8(446 + i * 16), type = v.U8(446 + i * 16 + 4);
        if (status != 0x00 && status != 0x80) return false;
        if (type) ++used;
    }
    return used > 0;
}
bool FatCheck(const View& v, uint64_t) {
    return v.Has(510, 2) && v.U16(510) == 0xAA55 && (std::memcmp(v.Data(54, 3), "FAT", 3) == 0 || std::memcmp(v.Data(82, 3), "FAT", 3) == 0);
}
bool Bz2Check(const View& v, uint64_t) { return v.Has(4, 6) && v.U8(3) >= '1' && v.U8(3) <= '9' && std::memcmp(v.Data(4, 6), "1AY&SY", 6) == 0; }
bool TarCheck(const View& v, uint64_t) { return v.Has(257, 5) && std::memcmp(v.Data(257, 5), "ustar", 5) == 0; }

// Ordered: longer and more specific magics first.
const std::vector<Signature>& Signatures() {
    static const std::vector<Signature> table = {
        // Images
        {0, B("\x89PNG\r\n\x1a\n"), "PNG image", "image", "image/png"},
        {0, B("\xff\xd8\xff"), "JPEG image", "image", "image/jpeg"},
        {0, B("GIF87a"), "GIF image", "image", "image/gif"}, {0, B("GIF89a"), "GIF image", "image", "image/gif"},
        {8, B("WEBP"), "WebP image", "image", "image/webp"},
        {0, B("II*\0"), "TIFF image", "image", "image/tiff"}, {0, B("MM\0*"), "TIFF image", "image", "image/tiff"},
        {0, B("8BPS"), "Photoshop document", "image", "image/vnd.adobe.photoshop"},
        {0, B("\0\0\1\0"), "Windows icon", "image", "image/vnd.microsoft.icon", IcoCheck},
        {0, B("\0\0\2\0"), "Windows cursor", "image", "image/x-win-bitmap", IcoCheck},
        {0, B("BM"), "BMP image", "image", "image/bmp", BmpCheck},
        {4, B("ftypheic"), "HEIF image", "image", "image/heic"}, {4, B("ftypavif"), "AVIF image", "image", "image/avif"},
        {0, B("DDS "), "DirectDraw Surface texture", "image", "image/vnd-ms.dds"},
        {0, B("\xabKTX 11\xbb"), "KTX texture", "image", "image/ktx"}, {0, B("\xabKTX 20\xbb"), "KTX2 texture", "image", "image/ktx2"},
        {0, B("VTF\0"), "Valve texture", "image", "application/octet-stream"},
        {0, B("\x76\x2f\x31\x01"), "OpenEXR image", "image", "image/x-exr"},
        {0, B("gimp xcf"), "GIMP image", "image", "image/x-xcf"},
        {36, B("acsp"), "ICC color profile", "image", "application/vnd.iccprofile"},
        // Audio and video
        {0, B("ID3"), "MP3 audio (ID3 tag)", "audio", "audio/mpeg"},
        {0, B("fLaC"), "FLAC audio", "audio", "audio/flac"},
        {0, B("OggS"), "Ogg container (Vorbis/Opus/Theora)", "audio", "audio/ogg"},
        {8, B("WAVE"), "WAV audio", "audio", "audio/wav"}, {8, B("AVI "), "AVI video", "video", "video/x-msvideo"},
        {8, B("AIFF"), "AIFF audio", "audio", "audio/aiff"},
        {0, B("MThd"), "MIDI music", "audio", "audio/midi"},
        {0, B("\x1a\x45\xdf\xa3"), "Matroska/WebM video", "video", "video/x-matroska"},
        {4, B("ftypqt"), "QuickTime movie", "video", "video/quicktime"},
        {4, B("ftypM4A"), "MPEG-4 audio", "audio", "audio/mp4"},
        {4, B("ftyp"), "MPEG-4 / ISO media", "video", "video/mp4", FtypCheck},
        {0, B("FLV\x01"), "Flash video", "video", "video/x-flv"},
        {0, B("BIK"), "Bink video", "video", "video/x-bink"}, {0, B("KB2"), "Bink 2 video", "video", "video/x-bink"},
        {0, B("BKHD"), "Wwise sound bank", "audio", "application/octet-stream"},
        {0, B("AKPK"), "Wwise package", "audio", "application/octet-stream"},
        {8, B("FEV "), "FMOD event bank", "audio", "application/octet-stream"},
        // Fonts
        {0, B("OTTO"), "OpenType font (CFF)", "font", "font/otf"},
        {0, B("\0\1\0\0"), "TrueType font", "font", "font/ttf", TtfCheck},
        {0, B("true"), "TrueType font (Apple)", "font", "font/ttf", TtfCheck},
        {0, B("ttcf"), "TrueType font collection", "font", "font/collection"},
        {0, B("wOFF"), "WOFF web font", "font", "font/woff"}, {0, B("wOF2"), "WOFF2 web font", "font", "font/woff2"},
        // Archives and compression
        {0, B("7z\xbc\xaf\x27\x1c"), "7-Zip archive", "archive", "application/x-7z-compressed"},
        {0, B("Rar!\x1a\x07\x01\x00"), "RAR 5 archive", "archive", "application/vnd.rar"},
        {0, B("Rar!\x1a\x07\x00"), "RAR archive", "archive", "application/vnd.rar"},
        {0, B("\xfd" "7zXZ\0"), "XZ compressed data", "archive", "application/x-xz"},
        {0, B("BZh"), "bzip2 compressed data", "archive", "application/x-bzip2", Bz2Check},
        {0, B("\x28\xb5\x2f\xfd"), "Zstandard compressed data", "archive", "application/zstd"},
        {0, B("\x04\x22\x4d\x18"), "LZ4 frame", "archive", "application/x-lz4"},
        {0, B("LZIP"), "lzip compressed data", "archive", "application/x-lzip"},
        {0, B("070701"), "cpio archive", "archive", "application/x-cpio"}, {0, B("070707"), "cpio archive", "archive", "application/x-cpio"},
        {0, B("xar!"), "XAR archive (macOS .pkg)", "archive", "application/x-xar"},
        {0, B("MSWIM\0\0\0"), "Windows imaging (WIM)", "archive", "application/x-ms-wim"},
        {0, B("ITSF"), "Compiled HTML help (CHM)", "document", "application/vnd.ms-htmlhelp"},
        {0, B("\xed\xab\xee\xdb"), "RPM package", "archive", "application/x-rpm"},
        {0, B("ANDROID BACKUP"), "Android backup", "archive", "application/octet-stream"},
        {257, B("ustar"), "TAR archive", "archive", "application/x-tar", TarCheck},
        // Disk images and file systems
        {32769, B("CD001"), "ISO 9660 disc image", "disk", "application/x-iso9660-image"},
        {0, B("conectix"), "Virtual PC disk (VHD)", "disk", "application/x-vhd"},
        {0, B("vhdxfile"), "Hyper-V disk (VHDX)", "disk", "application/x-vhdx"},
        {0, B("KDMV"), "VMware disk (VMDK)", "disk", "application/x-vmdk"},
        {0, B("# Disk DescriptorFile"), "VMware disk descriptor", "disk", "text/plain"},
        {0, B("QFI\xfb"), "QEMU disk (QCOW)", "disk", "application/x-qemu-disk"},
        {64, B("\x7f\x10\xda\xbe"), "VirtualBox disk (VDI)", "disk", "application/x-virtualbox-vdi"},
        {512, B("EFI PART"), "GPT partitioned disk", "disk", "application/octet-stream"},
        {3, B("NTFS    "), "NTFS volume boot sector", "disk", "application/octet-stream"},
        {3, B("-FVE-FS-"), "BitLocker volume (encrypted)", "disk", "application/octet-stream"},
        {0, B("LUKS\xba\xbe"), "LUKS encrypted volume", "disk", "application/octet-stream"},
        {1080, B("\x53\xef"), "ext2/3/4 file system", "disk", "application/octet-stream", Ext2Check},
        {0, B("hsqs"), "SquashFS file system", "disk", "application/vnd.squashfs"},
        {0, B("sqsh"), "SquashFS file system (big-endian)", "disk", "application/vnd.squashfs"},
        {0, B("\x45\x3d\xcd\x28"), "CramFS file system", "disk", "application/octet-stream"},
        {0, B("UBI#"), "UBI image", "disk", "application/octet-stream"},
        {0, B("-rom1fs-"), "romfs file system", "disk", "application/octet-stream"},
        {0, B("\x27\x05\x19\x56"), "U-Boot legacy image (uImage)", "firmware", "application/octet-stream"},
        {40, B("_FVH"), "UEFI firmware volume", "firmware", "application/octet-stream"},
        {0, B("ANDROID!"), "Android boot image", "firmware", "application/octet-stream"},
        {0, B("VNDRBOOT"), "Android vendor boot image", "firmware", "application/octet-stream"},
        {0, B("\x3a\xff\x26\xed"), "Android sparse image", "firmware", "application/octet-stream"},
        {0, B("AVB0"), "Android verified boot metadata (vbmeta)", "firmware", "application/octet-stream"},
        {0, B("\x55\xaa"), "Option ROM", "firmware", "application/octet-stream", [](const View& v, uint64_t size) { return v.Has(2, 1) && v.U8(2) && size >= uint64_t(v.U8(2)) * 512; }},
        {0, B("\xeb"), "FAT volume boot sector", "disk", "application/octet-stream", FatCheck},
        {510, B("\x55\xaa"), "MBR partitioned disk", "disk", "application/octet-stream", MbrCheck},
        // Android and managed code
        {0, B("dex\n0"), "Android DEX bytecode", "code", "application/vnd.android.dex"},
        {0, B("dey\n0"), "Android optimized DEX (ODEX)", "code", "application/octet-stream"},
        {0, B("vdex"), "Android VDEX", "code", "application/octet-stream"},
        {0, B("art\n"), "Android ART image", "code", "application/octet-stream"},
        {0, B("\xac\xed\x00\x05"), "Java serialized object", "code", "application/x-java-serialized-object"},
        {0, B("\xce\xca\xef\xbe"), ".NET resources (.resources)", "code", "application/octet-stream"},
        {0, B("\xc6\x1f\xbc\x03\xc1\x03\x19\x1f"), "Hermes bytecode (React Native)", "code", "application/octet-stream"},
        {0, B("\x04\x08"), "Ruby Marshal data", "code", "application/octet-stream", [](const View& v, uint64_t) { return v.Has(2, 1) && std::strchr("[{\"io:TF0", static_cast<char>(v.U8(2))) != nullptr && v.U8(2) != 0; }},
        // Game engines
        {0, B("UnityFS\0"), "Unity asset bundle (UnityFS)", "game", "application/octet-stream"},
        {0, B("UnityWeb\0"), "Unity web bundle", "game", "application/octet-stream"},
        {0, B("UnityRaw\0"), "Unity raw bundle", "game", "application/octet-stream"},
        {0, B("\xaf\x1b\xb1\xfa"), "Unity IL2CPP metadata (global-metadata.dat)", "game", "application/octet-stream"},
        {0, B("GDPC"), "Godot resource pack (.pck)", "game", "application/octet-stream"},
        {0, B("\xc1\x83\x2a\x9e"), "Unreal package (.uasset/.umap)", "game", "application/octet-stream"},
        {0, B("-==--==--==--==-"), "Unreal IoStore table of contents (.utoc)", "game", "application/octet-stream"},
        {0, B("FORM"), "IFF container (GameMaker data.win, AIFF...)", "game", "application/octet-stream"},
        {0, B("RGSSAD\0"), "RPG Maker encrypted archive (not decrypted)", "game", "application/octet-stream"},
        {0, B("XNB"), "XNA/MonoGame content (.xnb)", "game", "application/octet-stream"},
        {0, B("BSA\0"), "Bethesda archive (BSA)", "game", "application/octet-stream"},
        {0, B("BTDX"), "Bethesda archive (BA2)", "game", "application/octet-stream"},
        {0, B("TES4"), "Bethesda plugin (.esp/.esm)", "game", "application/octet-stream"},
        {0, B("VBSP"), "Source engine map (BSP)", "game", "application/octet-stream"},
        {0, B("IDST"), "Source engine model (MDL)", "game", "application/octet-stream"},
        {0, B("glTF"), "glTF binary model (.glb)", "3d", "model/gltf-binary"},
        {0, B("Kaydara FBX Binary"), "Autodesk FBX model", "3d", "application/octet-stream"},
        {0, B("BLENDER"), "Blender file", "3d", "application/x-blender"},
        {0, B("DXBC"), "Direct3D shader bytecode (DXBC)", "code", "application/octet-stream"},
        {0, B("MTLB"), "Metal shader library", "code", "application/octet-stream"},
        // Documents and mail
        {0, B("{\\rtf"), "RTF document", "document", "application/rtf"},
        {0, B("%!PS"), "PostScript document", "document", "application/postscript"},
        {0, B("\xc5\xd0\xd3\xc6"), "Encapsulated PostScript (binary)", "document", "application/postscript"},
        {0, B("AT&TFORM"), "DjVu document", "document", "image/vnd.djvu"},
        {60, B("BOOKMOBI"), "Mobipocket e-book", "document", "application/x-mobipocket-ebook"},
        {0, B("\xe4\x52\x5c\x7b\x8c\xd8\xa7\x4d"), "OneNote section", "document", "application/onenote"},
        {0, B("!BDN"), "Outlook data file (PST/OST)", "document", "application/vnd.ms-outlook"},
        {0, B("BEGIN:VCARD"), "vCard contact", "document", "text/vcard"},
        {0, B("BEGIN:VCALENDAR"), "iCalendar", "document", "text/calendar"},
        {0, B("d8:announce"), "BitTorrent file", "document", "application/x-bittorrent"},
        {0, B("[InternetShortcut]"), "Internet shortcut (.url)", "document", "text/plain"},
        {0, B("Windows Registry Editor Version 5.00"), "Registry export (.reg)", "document", "text/plain"},
        {0, B("REGEDIT4"), "Registry export (.reg, ANSI)", "document", "text/plain"},
        // Windows system files
        {0, B("ElfFile\0"), "Windows event log (EVTX)", "system", "application/octet-stream"},
        {4, B("LfLe"), "Windows event log (EVT, legacy)", "system", "application/octet-stream"},
        {4, B("SCCA"), "Windows Prefetch file", "system", "application/octet-stream"},
        {0, B("MAM\x04"), "Windows Prefetch file (compressed)", "system", "application/octet-stream"},
        {0, B("PML_"), "Process Monitor log (PML)", "system", "application/octet-stream"},
        {0, B("hibr"), "Windows hibernation file", "system", "application/octet-stream"},
        {0, B("HIBR"), "Windows hibernation file", "system", "application/octet-stream"},
        {0, B("PAGEDU64"), "Windows crash dump (full)", "system", "application/octet-stream"},
        {0, B("PAGEDUMP"), "Windows crash dump (full, 32-bit)", "system", "application/octet-stream"},
        {0, B("?_\x03\0"), "Windows help (HLP)", "document", "application/winhlp"},
        // Databases and data science
        {0, B("\0\1\0\0Standard Jet DB"), "Microsoft Access database (MDB)", "database", "application/x-msaccess"},
        {0, B("\0\1\0\0Standard ACE DB"), "Microsoft Access database (ACCDB)", "database", "application/x-msaccess"},
        {0, B("T-DB"), "Realm database", "database", "application/octet-stream"},
        {0, B("PAR1"), "Apache Parquet", "database", "application/vnd.apache.parquet"},
        {0, B("Obj\x01"), "Apache Avro", "database", "application/avro"},
        {0, B("ORC"), "Apache ORC", "database", "application/octet-stream"},
        {0, B("ARROW1"), "Apache Arrow", "database", "application/vnd.apache.arrow.file"},
        {0, B("\x89HDF\r\n\x1a\n"), "HDF5 data (Keras .h5, scientific)", "database", "application/x-hdf5"},
        {0, B("CDF\x01"), "NetCDF data", "database", "application/x-netcdf"},
        {0, B("MATLAB 5.0 MAT-file"), "MATLAB data", "database", "application/x-matlab-data"},
        {4, B("TFL3"), "TensorFlow Lite model", "ai", "application/octet-stream"},
        {0, B("lmgg"), "GGML model (legacy)", "ai", "application/octet-stream"},
        {0, B("ggjt"), "GGJT model (legacy llama.cpp)", "ai", "application/octet-stream"},
        {0, B("\xd9\xd9\xf7"), "CBOR data", "database", "application/cbor"},
        // Keys, certificates and encrypted containers
        {0, B("-----BEGIN PGP"), "OpenPGP armored data", "crypto", "application/pgp-encrypted"},
        {0, B("-----BEGIN OPENSSH PRIVATE KEY-----"), "OpenSSH private key", "crypto", "text/plain"},
        {0, B("ssh-rsa "), "SSH public key", "crypto", "text/plain"}, {0, B("ssh-ed25519 "), "SSH public key", "crypto", "text/plain"},
        {0, B("PuTTY-User-Key-File"), "PuTTY private key", "crypto", "text/plain"},
        {0, B("\xfe\xed\xfe\xed"), "Java keystore (JKS)", "crypto", "application/x-java-keystore"},
        {0, B("\xce\xce\xce\xce"), "Java keystore (JCEKS)", "crypto", "application/x-java-jce-keystore"},
        {0, B("\x03\xd9\xa2\x9a\x67\xfb\x4b\xb5"), "KeePass database", "crypto", "application/x-keepass2"},
        {0, B("age-encryption.org/v1"), "age encrypted file", "crypto", "application/octet-stream"},
        // Executables not claimed by a dedicated provider
        {0, B("MZ"), "DOS/Windows executable (no valid PE header)", "executable", "application/x-dosexec"},
        {0, B("#!"), "Script with an interpreter line", "script", "text/x-script"},
    };
    return table;
}

#undef B

bool Matches(const View& v, uint64_t size, const Signature& s) {
    if (!v.Has(s.offset, s.bytes.size())) return false;
    if (std::memcmp(v.Data(s.offset, s.bytes.size()), s.bytes.data(), s.bytes.size()) != 0) return false;
    return !s.check || s.check(v, size);
}

// What an extension usually is, when nothing in the bytes says more.
const char* ExtensionHint(const std::string& ext) {
    static const std::map<std::string, const char*> hints = {
        {"bin", "raw binary data, firmware or a dump"}, {"dat", "application data (format chosen by the program)"},
        {"pak", "game or application package"}, {"arc", "archive (format depends on the program)"},
        {"bnk", "sound bank (Wwise, FMOD)"}, {"pck", "package (Godot pack or Wwise package)"},
        {"assets", "Unity serialized assets"}, {"resource", "Unity resource data"}, {"ress", "Unity streamed resource data"},
        {"bundle", "asset bundle"}, {"unity3d", "Unity web player bundle"}, {"uasset", "Unreal asset"}, {"umap", "Unreal map"},
        {"uexp", "Unreal export data"}, {"ubulk", "Unreal bulk data"}, {"utoc", "Unreal IoStore table of contents"},
        {"ucas", "Unreal IoStore container"}, {"sav", "game save (format chosen by the game)"},
        {"save", "game save (format chosen by the game)"}, {"cfg", "configuration text"}, {"ini", "INI configuration"},
        {"conf", "configuration text"}, {"log", "log text"}, {"tmp", "temporary file"}, {"bak", "backup copy"},
        {"old", "previous copy"}, {"cache", "cache data"}, {"db", "database (often SQLite)"}, {"mdb", "Microsoft Access database"},
        {"ldb", "LevelDB table or Access lock file"}, {"sst", "RocksDB/LevelDB sorted table"}, {"blob", "binary large object"},
        {"key", "key material or Keynote document"}, {"pfx", "PKCS#12 certificate and key"}, {"p12", "PKCS#12 certificate and key"},
        {"kdbx", "KeePass database"}, {"gpg", "OpenPGP data"}, {"asc", "OpenPGP armored data"}, {"enc", "encrypted data"},
        {"iso", "disc image"}, {"img", "disk or firmware image"}, {"vhd", "virtual disk"}, {"vmdk", "virtual disk"},
        {"qcow2", "virtual disk"}, {"rom", "firmware image"}, {"fw", "firmware image"}, {"hex", "Intel HEX firmware"},
        {"elf", "ELF executable"}, {"so", "shared library"}, {"ko", "Linux kernel module"}, {"o", "object file"},
        {"dylib", "macOS dynamic library"}, {"app", "application bundle"}, {"ipa", "iOS application"}, {"apk", "Android application"},
        {"aab", "Android app bundle"}, {"xapk", "Android application with data"}, {"obb", "Android expansion data"},
        {"dex", "Android bytecode"}, {"oat", "Android compiled code"}, {"jar", "Java archive"}, {"class", "Java class"},
        {"swf", "Flash animation"}, {"fla", "Flash source"}, {"psd", "Photoshop document"}, {"ai", "Illustrator document"},
        {"svg", "vector image (XML)"}, {"tga", "Targa image"}, {"pcx", "PCX image"}, {"exr", "OpenEXR image"},
        {"hdr", "Radiance HDR image"}, {"blend", "Blender scene"}, {"fbx", "FBX model"}, {"obj", "object file or 3D model"},
        {"stl", "3D printing model"}, {"gltf", "glTF model (JSON)"}, {"glb", "glTF model (binary)"}, {"mdl", "model"},
        {"vtf", "Valve texture"}, {"vmt", "Valve material (text)"}, {"bsp", "map (Quake/Source)"}, {"wav", "audio"},
        {"ogg", "Ogg audio"}, {"opus", "Opus audio"}, {"flac", "FLAC audio"}, {"mp3", "MP3 audio"}, {"m4a", "MPEG-4 audio"},
        {"mp4", "MPEG-4 video"}, {"mkv", "Matroska video"}, {"webm", "WebM video"}, {"mov", "QuickTime movie"},
        {"avi", "AVI video"}, {"bik", "Bink video"}, {"usm", "CRI video"}, {"acb", "CRI audio cue bank"}, {"awb", "CRI audio wave bank"},
        {"ttf", "TrueType font"}, {"otf", "OpenType font"}, {"woff", "web font"}, {"woff2", "web font"},
        {"evtx", "Windows event log"}, {"etl", "Windows event trace"}, {"pf", "Windows Prefetch"}, {"pml", "Process Monitor log"},
        {"evt", "legacy Windows event log"}, {"reg", "registry export"}, {"msg", "Outlook message"}, {"pst", "Outlook data file"},
        {"eml", "e-mail message"}, {"mbox", "mailbox"}, {"ics", "calendar"}, {"vcf", "contact card"},
        {"doc", "Word document (OLE)"}, {"xls", "Excel workbook (OLE)"}, {"ppt", "PowerPoint (OLE)"}, {"docx", "Word document (ZIP)"},
        {"xlsx", "Excel workbook (ZIP)"}, {"pptx", "PowerPoint (ZIP)"}, {"odt", "OpenDocument text"}, {"epub", "e-book (ZIP)"},
        {"rtf", "RTF document"}, {"chm", "compiled help"}, {"hlp", "Windows help"}, {"xps", "XPS document"},
        {"torrent", "BitTorrent file"}, {"url", "Internet shortcut"}, {"desktop", "Linux desktop entry"},
        {"plist", "property list"}, {"strings", "macOS localized strings"}, {"nib", "macOS interface file"},
        {"car", "macOS asset catalog"}, {"mobileprovision", "iOS provisioning profile"}, {"pem", "PEM certificate or key"},
        {"crt", "certificate"}, {"der", "certificate (DER)"}, {"csr", "certificate request"}, {"jks", "Java keystore"},
        {"parquet", "Parquet data"}, {"avro", "Avro data"}, {"orc", "ORC data"}, {"feather", "Arrow data"}, {"h5", "HDF5 data"},
        {"hdf5", "HDF5 data"}, {"nc", "NetCDF data"}, {"mat", "MATLAB data"}, {"tflite", "TensorFlow Lite model"},
        {"pb", "Protocol Buffers data or TensorFlow graph"}, {"proto", "Protocol Buffers schema"}, {"mlmodel", "Core ML model"},
        {"bin", "raw binary data"}, {"ckpt", "model checkpoint"}, {"pt", "PyTorch checkpoint"}, {"pth", "PyTorch checkpoint"},
        {"wasm", "WebAssembly"}, {"map", "source map or linker map"}, {"lua", "Lua source"}, {"luac", "Lua bytecode"},
        {"rpyc", "Ren'Py compiled script"}, {"rgssad", "RPG Maker archive"}, {"rgss3a", "RPG Maker archive"},
        {"rvdata2", "RPG Maker data (Ruby Marshal)"}, {"xnb", "XNA content"}, {"esp", "Bethesda plugin"}, {"esm", "Bethesda master"},
        {"ba2", "Bethesda archive"}, {"bsa", "Bethesda archive"}, {"dds", "DirectDraw texture"}, {"ktx", "KTX texture"},
        {"spv", "SPIR-V shader"}, {"cso", "compiled HLSL shader"}, {"fxc", "compiled shader"}, {"metallib", "Metal library"},
    };
    const auto it = hints.find(ext);
    return it == hints.end() ? nullptr : it->second;
}

std::string Extension(const std::string& name) {
    const size_t slash = name.find_last_of("/\\!");
    const std::string base = slash == std::string::npos ? name : name.substr(slash + 1);
    const size_t dot = base.find_last_of('.');
    if (dot == std::string::npos || dot + 1 >= base.size() || base.size() - dot > 17) return "";
    std::string ext = base.substr(dot + 1);
    for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return ext;
}

double Entropy(const uint64_t counts[256], uint64_t total) {
    if (!total) return 0;
    double h = 0;
    for (int i = 0; i < 256; ++i) if (counts[i]) { const double p = double(counts[i]) / double(total); h -= p * std::log2(p); }
    return std::round(h * 1000) / 1000;
}

json EntropyProfile(const ByteSource& source) {
    const uint64_t size = std::min<uint64_t>(source.Size(), kEntropyLimit);
    const uint64_t block = std::max<uint64_t>(4096, (size + 63) / 64);
    uint64_t total[256] = {};
    json blocks = json::array();
    unsigned high = 0, low = 0;
    for (uint64_t at = 0; at < size; at += block) {
        const auto bytes = source.ReadSome(at, static_cast<size_t>(std::min<uint64_t>(block, size - at)));
        if (bytes.empty()) break;
        uint64_t counts[256] = {};
        for (uint8_t b : bytes) { ++counts[b]; ++total[b]; }
        const double e = Entropy(counts, bytes.size());
        if (e > 7.5) ++high; else ++low;
        blocks.push_back({{"offset", at}, {"size", bytes.size()}, {"entropy", e}});
    }
    const double overall = Entropy(total, size);
    std::string assessment = overall > 7.9 ? "compressed_or_encrypted" : high && low ? "mixed" : overall < 1.0 ? "sparse_or_padding" : "structured";
    return {{"overall", overall}, {"assessment", assessment}, {"block_size", block}, {"blocks", blocks},
            {"bytes_measured", size}};
}

// ---------------------------------------------------------------- text ---

json TextProfile(const std::vector<uint8_t>& bytes, uint64_t fileSize) {
    const size_t n = bytes.size();
    std::string encoding, bom;
    size_t start = 0;
    if (n >= 3 && bytes[0] == 0xEF && bytes[1] == 0xBB && bytes[2] == 0xBF) { encoding = "utf-8"; bom = "utf-8"; start = 3; }
    else if (n >= 2 && bytes[0] == 0xFF && bytes[1] == 0xFE) { encoding = "utf-16le"; bom = "utf-16le"; start = 2; }
    else if (n >= 2 && bytes[0] == 0xFE && bytes[1] == 0xFF) { encoding = "utf-16be"; bom = "utf-16be"; start = 2; }
    std::string text;
    if (encoding.empty() && n >= 8) {
        // UTF-16 without a BOM: one byte of each pair is mostly zero.
        size_t evenZero = 0, oddZero = 0, pairs = std::min<size_t>(n / 2, 4096);
        for (size_t i = 0; i < pairs; ++i) { evenZero += bytes[i * 2] == 0; oddZero += bytes[i * 2 + 1] == 0; }
        if (oddZero > pairs * 9 / 10 && evenZero < pairs / 10) encoding = "utf-16le";
        else if (evenZero > pairs * 9 / 10 && oddZero < pairs / 10) encoding = "utf-16be";
    }
    if (encoding == "utf-16le" || encoding == "utf-16be") {
        const size_t len = (n - start) & ~size_t(1);
        text = bounded::Utf16(View(bytes.data() + start, len), encoding == "utf-16le");
    } else {
        text = bounded::Utf8(bytes.data() + start, n - start);
        if (encoding.empty()) {
            bool ascii = true;
            for (size_t i = start; i < n && ascii; ++i) ascii = bytes[i] < 0x80;
            const bool validUtf8 = text.find("\xEF\xBF\xBD") == std::string::npos;  // no byte had to be replaced
            encoding = ascii ? "ascii" : validUtf8 ? "utf-8" : "binary";
        }
    }
    size_t printable = 0, controls = 0, nul = 0, crlf = 0, lf = 0, cr = 0, lines = 0, longest = 0, current = 0;
    for (size_t i = 0; i < text.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        if (c == 0) ++nul;
        if (c == '\n') { ++lf; ++lines; longest = std::max(longest, current); current = 0; if (i && text[i - 1] == '\r') ++crlf; continue; }
        if (c == '\r') { if (i + 1 >= text.size() || text[i + 1] != '\n') { ++cr; ++lines; longest = std::max(longest, current); current = 0; } continue; }
        ++current;
        if (c >= 32 || c == '\t' || c >= 0x80) ++printable; else ++controls;
    }
    longest = std::max(longest, current);
    const size_t total = std::max<size_t>(1, printable + controls + nul);
    const double ratio = double(printable) / double(total);
    const bool isText = encoding != "binary" && nul == 0 && ratio > 0.95;
    json out = {{"kind", isText ? "text" : "binary"}, {"printable_ratio", std::round(ratio * 1000) / 1000}};
    if (!isText) return out;
    if (!current || text.back() == '\n') {} else ++lines;
    out["encoding"] = encoding;
    if (!bom.empty()) out["byte_order_mark"] = bom;
    out["line_endings"] = crlf && crlf == lf ? "crlf" : crlf ? "mixed" : lf ? "lf" : cr ? "cr" : "none";
    out["lines"] = lines;
    out["longest_line"] = longest;
    if (bytes.size() < fileSize) out["note"] = "measured on the first " + std::to_string(bytes.size()) + " bytes";
    // Likely language.
    std::string trimmed = text.substr(0, 65536);
    const size_t first = trimmed.find_first_not_of(" \t\r\n");
    std::string kind = "plain text";
    json details = json::object();
    const auto startsWith = [&](const char* p) { return first != std::string::npos && trimmed.compare(first, std::strlen(p), p) == 0; };
    if (first != std::string::npos && (trimmed[first] == '{' || trimmed[first] == '[') && bytes.size() == fileSize && fileSize <= (16u << 20)) {
        try {
            const json doc = json::parse(text);
            kind = "JSON";
            if (doc.is_object()) { json keys = json::array(); for (const auto& [k, v] : doc.items()) { if (keys.size() >= 32) break; keys.push_back(k); } details["top_level_keys"] = keys; }
            else details["top_level_items"] = doc.size();
        } catch (const json::exception&) {}
    }
    if (kind == "plain text") {
        if (startsWith("#!")) {
            const std::string line = trimmed.substr(first, trimmed.find('\n', first) - first);
            kind = "script";
            details["interpreter"] = bounded::Clip(line.substr(2), 200);
        } else if (startsWith("<?xml") || startsWith("<!DOCTYPE") || startsWith("<")) {
            std::string lower = trimmed.substr(first, 512);
            for (char& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            kind = lower.find("<html") != std::string::npos || lower.find("<!doctype html") != std::string::npos ? "HTML"
                 : lower.find("<svg") != std::string::npos ? "SVG (XML)" : "XML";
        } else if (startsWith("Windows Registry Editor") || startsWith("REGEDIT4")) {
            kind = "registry export";
        } else if (startsWith("@echo off") || startsWith("@ECHO OFF")) {
            kind = "batch script";
        } else if (startsWith("---") || trimmed.find(":\n") != std::string::npos || trimmed.find(": ") != std::string::npos) {
            // YAML or INI or CSV: decide on line shapes.
            unsigned sections = 0, assignments = 0, colons = 0, commaLines = 0, linesSeen = 0, firstCommas = 0;
            bool sameCommas = true;
            size_t at = 0;
            while (at < trimmed.size() && linesSeen < 200) {
                size_t end = trimmed.find('\n', at);
                if (end == std::string::npos) end = trimmed.size();
                std::string line = trimmed.substr(at, end - at);
                at = end + 1;
                if (line.empty() || line == "\r") continue;
                ++linesSeen;
                if (line.front() == '[' && line.find(']') != std::string::npos) ++sections;
                if (line.find('=') != std::string::npos) ++assignments;
                if (line.find(": ") != std::string::npos || (!line.empty() && line.back() == ':')) ++colons;
                const unsigned commas = static_cast<unsigned>(std::count(line.begin(), line.end(), ','));
                if (linesSeen == 1) firstCommas = commas; else if (commas != firstCommas) sameCommas = false;
                if (commas) ++commaLines;
            }
            if (sections && assignments >= sections) kind = "INI";
            else if (firstCommas && sameCommas && linesSeen >= 2) kind = "CSV";
            else if (colons * 2 >= linesSeen || startsWith("---")) kind = "YAML";
        } else if (startsWith("# ") || trimmed.find("\n## ") != std::string::npos) {
            kind = "Markdown";
        }
        if (kind == "plain text") {
            // Not a whole file of key=value lines? Try INI and CSV anyway.
            unsigned sections = 0, assignments = 0, linesSeen = 0, firstCommas = 0;
            bool sameCommas = true;
            size_t at = 0;
            while (at < trimmed.size() && linesSeen < 200) {
                size_t end = trimmed.find('\n', at);
                if (end == std::string::npos) end = trimmed.size();
                const std::string line = trimmed.substr(at, end - at);
                at = end + 1;
                if (line.empty() || line == "\r") continue;
                ++linesSeen;
                if (line.front() == '[' && line.find(']') != std::string::npos) ++sections;
                if (line.find('=') != std::string::npos) ++assignments;
                const unsigned commas = static_cast<unsigned>(std::count(line.begin(), line.end(), ','));
                if (linesSeen == 1) firstCommas = commas; else if (commas != firstCommas) sameCommas = false;
            }
            if (sections && assignments >= sections) kind = "INI";
            else if (firstCommas && sameCommas && linesSeen >= 2) kind = "CSV";
        }
    }
    out["language"] = kind;
    if (!details.empty()) out["details"] = details;
    // A bounded preview: the first lines.
    json preview = json::array();
    size_t at = 0;
    while (at < text.size() && preview.size() < 12) {
        size_t end = text.find('\n', at);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(at, end - at);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        preview.push_back(bounded::Clip(line, 200));
        at = end + 1;
    }
    out["preview"] = preview;
    return out;
}

// ------------------------------------------------------------- strings ---

struct Strings {
    std::vector<std::string> ascii, utf16;
    uint64_t asciiCount = 0, utf16Count = 0;
    std::map<std::string, std::set<std::string>> notable;

    static bool Printable(uint8_t c) { return c >= 0x20 && c < 0x7F; }

    void Note(const std::string& kind, const std::string& value) {
        auto& set = notable[kind];
        if (set.size() < 64) set.insert(bounded::Clip(value, 300));
    }

    void Classify(const std::string& s) {
        for (const char* scheme : {"http://", "https://", "ftp://", "wss://", "ws://", "file://"}) {
            size_t at = s.find(scheme);
            while (at != std::string::npos) {
                size_t end = at;
                while (end < s.size() && !std::isspace(static_cast<unsigned char>(s[end])) && s[end] != '"' && s[end] != '\'' && s[end] != '<' && s[end] != '>') ++end;
                if (end - at > std::strlen(scheme) + 3) Note("urls", s.substr(at, end - at));
                at = s.find(scheme, end);
            }
        }
        // E-mail addresses: local@domain.tld
        for (size_t at = s.find('@'); at != std::string::npos; at = s.find('@', at + 1)) {
            size_t b = at, e = at + 1;
            while (b > 0 && (std::isalnum(static_cast<unsigned char>(s[b - 1])) || std::strchr("._%+-", s[b - 1]))) --b;
            while (e < s.size() && (std::isalnum(static_cast<unsigned char>(s[e])) || s[e] == '.' || s[e] == '-')) ++e;
            const std::string domain = s.substr(at + 1, e - at - 1);
            const size_t dot = domain.rfind('.');
            if (b < at && dot != std::string::npos && dot > 0 && domain.size() - dot >= 3 && std::isalpha(static_cast<unsigned char>(domain.back())))
                Note("emails", s.substr(b, e - b));
        }
        // IPv4 addresses.
        for (size_t i = 0; i < s.size(); ++i) {
            if (!std::isdigit(static_cast<unsigned char>(s[i])) || (i && (std::isalnum(static_cast<unsigned char>(s[i - 1])) || s[i - 1] == '.'))) continue;
            size_t j = i; int parts = 0; bool ok = true;
            while (parts < 4 && ok) {
                size_t k = j; int value = 0;
                while (k < s.size() && std::isdigit(static_cast<unsigned char>(s[k])) && k - j < 3) value = value * 10 + (s[k++] - '0');
                ok = k > j && value <= 255;
                j = k; ++parts;
                if (parts < 4) { ok = ok && j < s.size() && s[j] == '.'; ++j; }
            }
            if (ok && parts == 4 && (j >= s.size() || !(std::isalnum(static_cast<unsigned char>(s[j])) || s[j] == '.'))) { Note("ip_addresses", s.substr(i, j - i)); i = j; }
        }
        // Windows paths, UNC paths, environment paths and Unix paths.
        if (s.size() > 3 && std::isalpha(static_cast<unsigned char>(s[0])) && s[1] == ':' && s[2] == '\\') Note("paths", s);
        else if (s.rfind("\\\\", 0) == 0 && s.size() > 4) Note("paths", s);
        else if (s.rfind("%", 0) == 0 && s.find("%\\") != std::string::npos) Note("paths", s);
        else if (s.size() > 5 && s[0] == '/' && s.find('/', 1) != std::string::npos && s.find(' ') == std::string::npos &&
                 std::count(s.begin(), s.end(), '/') >= 2) Note("paths", s);
        // Registry keys.
        for (const char* root : {"HKEY_", "HKLM\\", "HKCU\\", "HKCR\\", "SOFTWARE\\", "Software\\", "SYSTEM\\CurrentControlSet"}) {
            const size_t at = s.find(root);
            if (at != std::string::npos && (at == 0 || !std::isalnum(static_cast<unsigned char>(s[at - 1])))) { Note("registry_keys", s.substr(at)); break; }
        }
        // GUIDs.
        for (size_t i = 0; i + 36 <= s.size(); ++i) {
            bool ok = true;
            for (size_t k = 0; k < 36 && ok; ++k) {
                const char c = s[i + k];
                ok = (k == 8 || k == 13 || k == 18 || k == 23) ? c == '-' : std::isxdigit(static_cast<unsigned char>(c)) != 0;
            }
            if (ok) { Note("guids", s.substr(i, 36)); i += 35; }
        }
    }

    void Add(std::vector<std::string>& list, uint64_t& count, std::string s) {
        ++count;
        Classify(s);
        if (list.size() < 400) list.push_back(bounded::Clip(s, 200));
    }

    void Scan(const std::vector<uint8_t>& b, size_t minLength = 6) {
        std::string cur;
        for (size_t i = 0; i <= b.size(); ++i) {
            if (i < b.size() && (Printable(b[i]) || b[i] == '\t')) { if (cur.size() < 4096) cur += static_cast<char>(b[i]); continue; }
            if (cur.size() >= minLength) Add(ascii, asciiCount, cur);
            cur.clear();
        }
        for (size_t parity = 0; parity < 2; ++parity) {  // UTF-16LE at either alignment
            cur.clear();
            for (size_t i = parity; i + 1 <= b.size(); i += 2) {
                const bool ok = i + 1 < b.size() && Printable(b[i]) && b[i + 1] == 0;
                if (ok) { if (cur.size() < 4096) cur += static_cast<char>(b[i]); continue; }
                if (cur.size() >= minLength) Add(utf16, utf16Count, cur);
                cur.clear();
            }
        }
    }

    json Json() const {
        json n = json::object();
        for (const auto& [k, v] : notable) n[k] = v;
        return {{"ascii_count", asciiCount}, {"utf16_count", utf16Count}, {"notable", n},
                {"ascii_sample", ascii}, {"utf16_sample", utf16}};
    }
};

// ------------------------------------------------------------ embedded ---

struct Hit { uint64_t offset; std::string name; std::string provider; uint64_t size = 0; };  // size 0: up to the next hit

// Strong magics worth looking for inside a file; each hit is confirmed by a
// dedicated provider on the bytes from that offset, or by a long magic.
std::vector<Hit> Embedded(const std::vector<uint8_t>& b, size_t limit) {
    struct Probe { std::string bytes; const char* name; bool needsProvider; };
    static const std::vector<Probe> probes = {
        {std::string("MZ", 2), "Windows executable", true}, {std::string("PK\x03\x04", 4), "ZIP archive", true},
        {std::string("\x7f" "ELF", 4), "ELF executable", true}, {std::string("\x89PNG\r\n\x1a\n", 8), "PNG image", false},
        {std::string("\xff\xd8\xff\xe0", 4), "JPEG image", false}, {std::string("\xff\xd8\xff\xe1", 4), "JPEG image", false},
        {std::string("GIF89a", 6), "GIF image", false}, {std::string("%PDF-", 5), "PDF document", true},
        {std::string("SQLite format 3\0", 16), "SQLite database", true},
        {std::string("\xd0\xcf\x11\xe0\xa1\xb1\x1a\xe1", 8), "OLE compound file", true},
        {std::string("MSCF\0\0\0\0", 8), "CAB archive", true}, {std::string("\x1f\x8b\x08", 3), "gzip data", true},
        {std::string("7z\xbc\xaf\x27\x1c", 6), "7-Zip archive", false}, {std::string("Rar!\x1a\x07", 6), "RAR archive", false},
        {std::string("\xfd" "7zXZ\0", 6), "XZ data", false}, {std::string("\xca\xfe\xba\xbe", 4), "Java class or fat Mach-O", true},
        {std::string("dex\n0", 5), "Android DEX", false}, {std::string("\0asm\x01\0\0\0", 8), "WebAssembly module", true},
        {std::string("\xcf\xfa\xed\xfe", 4), "Mach-O binary", true}, {std::string("OggS\0", 5), "Ogg stream", false},
        {std::string("fLaC", 4), "FLAC audio", false}, {std::string("\x28\xb5\x2f\xfd", 4), "Zstandard data", false},
        {std::string("\xd0\x0d\xfe\xed", 4), "device tree", true}, {std::string("\x1bLua", 4), "Lua bytecode", true},
        {std::string("\xa1\xb2\xc3\xd4", 4), "pcap capture", true}, {std::string("\xd4\xc3\xb2\xa1", 4), "pcap capture", true},
        {std::string("bplist00", 8), "binary property list", true}, {std::string("UnityFS\0", 8), "Unity asset bundle", false},
        {std::string("GDPC", 4), "Godot pack", false},
    };
    const auto& registry = DefaultRegistry();
    std::vector<Hit> hits;
    std::set<uint64_t> seen;
    const ByteSourcePtr whole = FromBytes(b, "scan");  // confirmations read windows of it, no copies
    for (size_t i = 1; i < b.size() && hits.size() < limit; ++i) {
        for (const auto& p : probes) {
            if (b[i] != static_cast<uint8_t>(p.bytes[0]) || i + p.bytes.size() > b.size()) continue;
            if (std::memcmp(b.data() + i, p.bytes.data(), p.bytes.size()) != 0) continue;
            if (p.bytes[0] == '\x1f') {
                // Three magic bytes recur every ~16 MiB of random data: also
                // require a valid RFC 1952 header (reserved flag bits clear,
                // a defined XFL, a known OS byte).
                if (i + 10 > b.size() || (b[i + 3] & 0xe0) != 0) continue;
                if (b[i + 8] != 0 && b[i + 8] != 2 && b[i + 8] != 4) continue;
                if (b[i + 9] > 13 && b[i + 9] != 255) continue;
            }
            // Confirm with a dedicated provider on the bytes from here on.
            uint64_t exact = 0;
            if (p.bytes == std::string("PK\x03\x04", 4)) {
                // A ZIP ends with its end-of-central-directory record; cut there,
                // since whatever follows (another embedded file) is not part of it.
                static const std::string eocd("PK\x05\x06", 4);
                const auto at = std::search(b.begin() + static_cast<std::ptrdiff_t>(i), b.end(), eocd.begin(), eocd.end());
                if (at != b.end() && static_cast<size_t>(b.end() - at) >= 22) {
                    const size_t e = static_cast<size_t>(at - b.begin());
                    const size_t end = e + 22 + (b[e + 20] | (b[e + 21] << 8));
                    if (end <= b.size()) exact = end - i;
                }
            }
            const ByteSourcePtr tail = Slice(whole, i, exact ? exact : b.size() - i, "probe");
            std::string confirmed;
            int best = 0;
            for (const auto& provider : registry.Providers()) {
                if (std::strcmp(provider->Name(), "generic") == 0) continue;
                const int score = provider->Detect(*tail);
                if (score > best) { best = score; confirmed = provider->Name(); }
            }
            if (p.needsProvider && confirmed.empty()) continue;
            if (seen.insert(i).second) hits.push_back({i, p.name, confirmed, exact});
            break;
        }
    }
    return hits;
}

} // namespace

std::vector<Capability> GenericProvider::Capabilities() const {
    return {{"static", CapabilityStatus::Experimental, "cortex_format_more_tests; fixtures from ffmpeg, zip and Python, identified as by file(1)",
             "signature identification (~150 formats), extension hint, text profile, entropy profile, strings with URLs/IPs/paths/registry keys/GUIDs, embedded files confirmed by dedicated providers"},
            {"decompile", CapabilityStatus::Unavailable, "", "no dedicated reader for this format"},
            {"live", CapabilityStatus::Unavailable, "", "the file is never run"}};
}

int GenericProvider::Detect(const ByteSource&) const { return 1; }

json GenericProvider::Inspect(const ByteSource& source, const InspectOptions& options, std::string& error) const {
    error.clear();
    const uint64_t size = source.Size();
    const auto head = source.ReadSome(0, static_cast<size_t>(std::min<uint64_t>(size, 65536)));
    const View hv(head);
    json result = {{"format", "unknown"}, {"size", size},
                   {"note", "No dedicated reader claims this file; this is a generic analysis of its bytes."}};
    json signature = nullptr;
    for (const auto& s : Signatures()) {
        if (s.offset >= 65536) {  // e.g. ISO 9660 at 32769
            const auto at = source.ReadSome(s.offset, s.bytes.size());
            if (at.size() == s.bytes.size() && std::memcmp(at.data(), s.bytes.data(), s.bytes.size()) == 0) {
                signature = {{"name", s.name}, {"category", s.category}, {"mime", s.mime}, {"offset", s.offset}};
                break;
            }
            continue;
        }
        if (Matches(hv, size, s)) { signature = {{"name", s.name}, {"category", s.category}, {"mime", s.mime}, {"offset", s.offset}}; break; }
    }
    if (signature.is_null() && size >= 32769 + 5) {
        const auto iso = source.ReadSome(32769, 5);
        if (iso.size() == 5 && std::memcmp(iso.data(), "CD001", 5) == 0)
            signature = {{"name", "ISO 9660 disc image"}, {"category", "disk"}, {"mime", "application/x-iso9660-image"}, {"offset", 32769}};
    }
    result["signature"] = signature;
    const std::string ext = Extension(source.Name());
    if (!ext.empty()) {
        json e = {{"extension", ext}};
        if (const char* hint = ExtensionHint(ext)) e["usually"] = hint;
        result["extension"] = e;
    }
    result["entropy"] = EntropyProfile(source);
    const auto scan = source.ReadSome(0, static_cast<size_t>(std::min<uint64_t>(size, kScanLimit)));
    result["content"] = TextProfile(std::vector<uint8_t>(scan.begin(), scan.begin() + static_cast<std::ptrdiff_t>(std::min<size_t>(scan.size(), 4u << 20))), size);
    Strings strings;
    strings.Scan(scan);
    result["strings"] = strings.Json();
    json embedded = json::array();
    for (const auto& h : Embedded(scan, std::min<size_t>(options.maxEntries, 64))) {
        json row = {{"offset", h.offset}, {"offset_hex", HexU64(h.offset)}, {"kind", h.name}};
        if (!h.provider.empty()) row["provider"] = h.provider;
        embedded.push_back(std::move(row));
    }
    result["embedded"] = embedded;
    if (scan.size() < size) result["scanned_bytes"] = scan.size();
    // A one-line verdict for people and agents.
    std::string summary;
    if (!size) summary = "empty file";
    else if (!signature.is_null()) summary = signature["name"].get<std::string>();
    else if (result["content"]["kind"] == "text") summary = result["content"].value("language", std::string("text")) + " (" + result["content"].value("encoding", std::string()) + ")";
    else if (result["entropy"]["assessment"] == "compressed_or_encrypted") summary = "high-entropy data (compressed or encrypted)";
    else summary = "binary data of an unrecognized format";
    if (size && signature.is_null() && result.contains("extension") && result["extension"].contains("usually"))
        summary += "; the extension usually denotes " + result["extension"]["usually"].get<std::string>();
    if (!embedded.empty()) summary += "; " + std::to_string(embedded.size()) + " embedded item(s) found";
    result["summary"] = summary;
    return result;
}

std::vector<Child> GenericProvider::Children(const ByteSource& source, const InspectOptions& options, std::string& error) const {
    error.clear();
    const uint64_t size = source.Size();
    const auto scan = source.ReadSome(0, static_cast<size_t>(std::min<uint64_t>(size, kScanLimit)));
    const auto hits = Embedded(scan, std::min<size_t>(options.maxEntries, 64));
    std::vector<Child> children;
    for (size_t i = 0; i < hits.size(); ++i) {
        const uint64_t end = hits[i].size ? hits[i].offset + hits[i].size : i + 1 < hits.size() ? hits[i + 1].offset : size;
        Child c;
        c.name = "embedded@" + HexU64(hits[i].offset) + (hits[i].provider.empty() ? "" : "." + hits[i].provider);
        c.offset = hits[i].offset;
        c.size = end - hits[i].offset;
        c.note = hits[i].name + (hits[i].size ? "; exact size" : end == size ? "; up to the end of the file" : "; up to the next embedded item");
        children.push_back(std::move(c));
    }
    return children;
}

} // namespace cortex::artifacts

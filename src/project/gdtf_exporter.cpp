// gdtf_exporter.cpp — GDTF (DIN SPEC 15800:2022) exporter.
//
// Writes a self-contained ZIP archive with:
//   fixture_type.xml   — GDTF fixture definition
//   thumbnail.png      — 1×1 white PNG placeholder (required by some parsers)
//
// ZIP format reference: PKWARE Application Note version 6.3.10
// GDTF spec reference:  https://gdtf-share.com / DIN SPEC 15800
//
// No zlib dependency: the fixture_type.xml and thumbnail.png are small enough
// to be stored with DEFLATE compression method 0 (stored, no compression).
// This is valid per the ZIP specification and accepted by all GDTF importers.

#include "gdtf_exporter.h"

#include <cstring>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <string>
#include <vector>
#include <array>

#include "../core/logger.h"

namespace idhmfis {

// ─────────────────────────────────────────────────────────────────────────────
//  Minimal ZIP writer (stored, no compression)
// ─────────────────────────────────────────────────────────────────────────────
namespace {

// CRC-32 table (IEEE polynomial 0xEDB88320)
static uint32_t make_crc32_table_entry(int n) {
    uint32_t c = static_cast<uint32_t>(n);
    for (int k = 0; k < 8; ++k) {
        if (c & 1u) c = 0xEDB88320u ^ (c >> 1);
        else         c >>= 1;
    }
    return c;
}

static uint32_t crc32_update(uint32_t crc, const uint8_t* data, size_t len) {
    crc = ~crc;
    for (size_t i = 0; i < len; ++i) {
        uint8_t b = data[i];
        crc = (crc >> 8) ^ make_crc32_table_entry(static_cast<int>((crc ^ b) & 0xFF));
    }
    return ~crc;
}

// Little-endian helpers
static void put_u16(std::vector<uint8_t>& v, uint16_t x) {
    v.push_back(static_cast<uint8_t>(x & 0xFF));
    v.push_back(static_cast<uint8_t>((x >> 8) & 0xFF));
}

static void put_u32(std::vector<uint8_t>& v, uint32_t x) {
    v.push_back(static_cast<uint8_t>( x        & 0xFF));
    v.push_back(static_cast<uint8_t>((x >>  8) & 0xFF));
    v.push_back(static_cast<uint8_t>((x >> 16) & 0xFF));
    v.push_back(static_cast<uint8_t>((x >> 24) & 0xFF));
}

static void put_bytes(std::vector<uint8_t>& v, const uint8_t* d, size_t n) {
    v.insert(v.end(), d, d + n);
}

static void put_str(std::vector<uint8_t>& v, const std::string& s) {
    v.insert(v.end(), s.begin(), s.end());
}

// MS-DOS timestamp encoding for current time
static void get_msdos_time(uint16_t& date_out, uint16_t& time_out) {
    std::time_t now = std::time(nullptr);
    std::tm* lt = std::localtime(&now);
    if (!lt) { date_out = 0x4A21; time_out = 0x0000; return; }
    // MS-DOS date: bits 15-9 = year-1980, bits 8-5 = month, bits 4-0 = day
    date_out = static_cast<uint16_t>(
        (std::max(0, lt->tm_year - 80) << 9) |
        ((lt->tm_mon + 1)               << 5) |
         lt->tm_mday);
    // MS-DOS time: bits 15-11 = hours, bits 10-5 = minutes, bits 4-0 = sec/2
    time_out = static_cast<uint16_t>(
        (lt->tm_hour << 11) |
        (lt->tm_min  <<  5) |
        (lt->tm_sec  /   2));
}

struct ZipEntry {
    std::string           name;
    std::vector<uint8_t>  data;
    uint32_t              crc;
    uint32_t              local_offset;
    uint16_t              dos_date;
    uint16_t              dos_time;
};

// Build a ZIP archive from a list of entries (stored, method 0)
static std::vector<uint8_t> build_zip(std::vector<ZipEntry>& entries) {
    std::vector<uint8_t> out;
    out.reserve(4096);

    uint16_t dd, dt;
    get_msdos_time(dd, dt);

    // Write local file headers + data
    for (auto& e : entries) {
        e.local_offset = static_cast<uint32_t>(out.size());
        e.dos_date = dd;
        e.dos_time = dt;

        // Local file header signature
        put_u32(out, 0x04034B50u);
        put_u16(out, 20);            // version needed: 2.0
        put_u16(out, 0);             // general purpose bit flag
        put_u16(out, 0);             // compression method: stored
        put_u16(out, dt);            // last mod time
        put_u16(out, dd);            // last mod date
        put_u32(out, e.crc);         // CRC-32
        put_u32(out, static_cast<uint32_t>(e.data.size())); // compressed size
        put_u32(out, static_cast<uint32_t>(e.data.size())); // uncompressed size
        put_u16(out, static_cast<uint16_t>(e.name.size())); // file name length
        put_u16(out, 0);             // extra field length

        put_str(out, e.name);
        put_bytes(out, e.data.data(), e.data.size());
    }

    uint32_t cd_offset = static_cast<uint32_t>(out.size());

    // Central directory
    for (const auto& e : entries) {
        put_u32(out, 0x02014B50u);   // central dir signature
        put_u16(out, 20);            // version made by
        put_u16(out, 20);            // version needed
        put_u16(out, 0);             // general purpose bit flag
        put_u16(out, 0);             // compression method: stored
        put_u16(out, e.dos_time);    // last mod time
        put_u16(out, e.dos_date);    // last mod date
        put_u32(out, e.crc);
        put_u32(out, static_cast<uint32_t>(e.data.size())); // compressed size
        put_u32(out, static_cast<uint32_t>(e.data.size())); // uncompressed size
        put_u16(out, static_cast<uint16_t>(e.name.size())); // file name length
        put_u16(out, 0);             // extra field length
        put_u16(out, 0);             // file comment length
        put_u16(out, 0);             // disk number start
        put_u16(out, 0);             // internal attributes
        put_u32(out, 0);             // external attributes
        put_u32(out, e.local_offset); // relative offset of local header
        put_str(out, e.name);
    }

    uint32_t cd_size = static_cast<uint32_t>(out.size()) - cd_offset;

    // End of central directory record
    put_u32(out, 0x06054B50u);   // EOCD signature
    put_u16(out, 0);             // disk number
    put_u16(out, 0);             // disk with start of CD
    put_u16(out, static_cast<uint16_t>(entries.size())); // entries on this disk
    put_u16(out, static_cast<uint16_t>(entries.size())); // total entries
    put_u32(out, cd_size);       // size of central directory
    put_u32(out, cd_offset);     // offset of central directory
    put_u16(out, 0);             // ZIP comment length

    return out;
}

// ─────────────────────────────────────────────────────────────────────────────
//  GDTF fixture_type.xml content
// ─────────────────────────────────────────────────────────────────────────────
static const char* build_fixture_xml() {
    // Static buffer — large enough for the XML
    static char xml[16384];

    // GDTF fixture definition
    // DIN SPEC 15800:2022 / GDTF 1.2 schema
    // Each DMX channel maps to a GDTF ChannelFunction → DMXChannel.
    //
    // Pan/Tilt are 16-bit (two DMX channels each): Pan (Ch1 MSB, Ch2 LSB),
    // Tilt (Ch3 MSB, Ch4 LSB).  In GDTF, 16-bit channels are modelled as
    // two linked DMXChannels with Offset="1,2" syntax.

    const char* tmpl =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<GDTF DataVersion=\"1.2\">\n"
        "  <FixtureType\n"
        "    Name=\"IDHMFIS Laser Show Controller\"\n"
        "    ShortName=\"IDHMFIS\"\n"
        "    LongName=\"IDHMFIS Professional Laser Show Controller\"\n"
        "    Manufacturer=\"IDHMFIS\"\n"
        "    Description=\"64-channel professional laser show controller. "
        "Channels 1-10 active; 11-64 reserved.\"\n"
        "    FixtureTypeID=\"{A1B2C3D4-E5F6-7890-ABCD-EF1234567890}\"\n"
        "    Thumbnail=\"thumbnail\"\n"
        "    RefFT=\"\"\n"
        "    CanHaveChildren=\"No\">\n"
        "\n"
        "    <!-- Physical geometry -->\n"
        "    <Geometries>\n"
        "      <Geometry Name=\"Base\" Model=\"\" Position=\"{1,0,0,0}{0,1,0,0}{0,0,1,0}{0,0,0,1}\">\n"
        "        <Geometry Name=\"Head\" Model=\"\" Position=\"{1,0,0,0}{0,1,0,0}{0,0,1,0}{0,0,1,1}\">\n"
        "          <Beam Name=\"Beam\" Model=\"\" Position=\"{1,0,0,0}{0,1,0,0}{0,0,1,0}{0,0,0,1}\"\n"
        "            LampType=\"Laser\" PowerConsumption=\"30\"\n"
        "            LuminousFlux=\"1000\" ColorTemperature=\"6500\"\n"
        "            BeamAngle=\"0.1\" BeamRadius=\"0.01\"\n"
        "            FieldAngle=\"0.1\" MinimumDiameter=\"0.001\"\n"
        "            BeamType=\"Laser\"/>\n"
        "        </Geometry>\n"
        "      </Geometry>\n"
        "    </Geometries>\n"
        "\n"
        "    <!-- DMX Modes -->\n"
        "    <DMXModes>\n"
        "      <DMXMode Name=\"Default\" Geometry=\"Base\">\n"
        "        <DMXChannels>\n"
        "\n"
        "          <!-- Ch 1-2: Pan 16-bit (MSB, LSB) -->\n"
        "          <DMXChannel DMXBreak=\"1\" Offset=\"1,2\"\n"
        "            Default=\"32768/2\" Highlight=\"None\" Geometry=\"Head\">\n"
        "            <LogicalChannel Attribute=\"Pan\" Snap=\"No\"\n"
        "              Master=\"Grand\" MibFade=\"0\" DMXChangeTimeLimit=\"0\">\n"
        "              <ChannelFunction Attribute=\"Pan\" Name=\"Pan\"\n"
        "                OriginalAttribute=\"\" DMXFrom=\"0/2\" DMXTo=\"65535/2\"\n"
        "                PhysicalFrom=\"-540\" PhysicalTo=\"540\"\n"
        "                RealFade=\"0\" RealAcceleration=\"0\"\n"
        "                Wheel=\"\" Emitter=\"\" Filter=\"\"\n"
        "                ModeMaster=\"\" ModeFrom=\"0/1\" ModeTo=\"0/1\">\n"
        "                <ChannelSet Name=\"\" DMXFrom=\"0/2\" PhysicalFrom=\"-540\" PhysicalTo=\"540\"/>\n"
        "              </ChannelFunction>\n"
        "            </LogicalChannel>\n"
        "          </DMXChannel>\n"
        "\n"
        "          <!-- Ch 3-4: Tilt 16-bit (MSB, LSB) -->\n"
        "          <DMXChannel DMXBreak=\"1\" Offset=\"3,4\"\n"
        "            Default=\"32768/2\" Highlight=\"None\" Geometry=\"Head\">\n"
        "            <LogicalChannel Attribute=\"Tilt\" Snap=\"No\"\n"
        "              Master=\"Grand\" MibFade=\"0\" DMXChangeTimeLimit=\"0\">\n"
        "              <ChannelFunction Attribute=\"Tilt\" Name=\"Tilt\"\n"
        "                OriginalAttribute=\"\" DMXFrom=\"0/2\" DMXTo=\"65535/2\"\n"
        "                PhysicalFrom=\"-270\" PhysicalTo=\"270\"\n"
        "                RealFade=\"0\" RealAcceleration=\"0\"\n"
        "                Wheel=\"\" Emitter=\"\" Filter=\"\"\n"
        "                ModeMaster=\"\" ModeFrom=\"0/1\" ModeTo=\"0/1\">\n"
        "                <ChannelSet Name=\"\" DMXFrom=\"0/2\" PhysicalFrom=\"-270\" PhysicalTo=\"270\"/>\n"
        "              </ChannelFunction>\n"
        "            </LogicalChannel>\n"
        "          </DMXChannel>\n"
        "\n"
        "          <!-- Ch 5: Dimmer (Intensity) -->\n"
        "          <DMXChannel DMXBreak=\"1\" Offset=\"5\"\n"
        "            Default=\"0/1\" Highlight=\"255/1\" Geometry=\"Beam\">\n"
        "            <LogicalChannel Attribute=\"Dimmer\" Snap=\"No\"\n"
        "              Master=\"Grand\" MibFade=\"0\" DMXChangeTimeLimit=\"0\">\n"
        "              <ChannelFunction Attribute=\"Dimmer\" Name=\"Dimmer\"\n"
        "                OriginalAttribute=\"\" DMXFrom=\"0/1\" DMXTo=\"255/1\"\n"
        "                PhysicalFrom=\"0\" PhysicalTo=\"1\"\n"
        "                RealFade=\"0\" RealAcceleration=\"0\"\n"
        "                Wheel=\"\" Emitter=\"\" Filter=\"\"\n"
        "                ModeMaster=\"\" ModeFrom=\"0/1\" ModeTo=\"0/1\">\n"
        "                <ChannelSet Name=\"Open\" DMXFrom=\"0/1\" PhysicalFrom=\"0\" PhysicalTo=\"1\"/>\n"
        "              </ChannelFunction>\n"
        "            </LogicalChannel>\n"
        "          </DMXChannel>\n"
        "\n"
        "          <!-- Ch 6: Red -->\n"
        "          <DMXChannel DMXBreak=\"1\" Offset=\"6\"\n"
        "            Default=\"0/1\" Highlight=\"255/1\" Geometry=\"Beam\">\n"
        "            <LogicalChannel Attribute=\"ColorAdd_R\" Snap=\"No\"\n"
        "              Master=\"Grand\" MibFade=\"0\" DMXChangeTimeLimit=\"0\">\n"
        "              <ChannelFunction Attribute=\"ColorAdd_R\" Name=\"Red\"\n"
        "                OriginalAttribute=\"\" DMXFrom=\"0/1\" DMXTo=\"255/1\"\n"
        "                PhysicalFrom=\"0\" PhysicalTo=\"1\"\n"
        "                RealFade=\"0\" RealAcceleration=\"0\"\n"
        "                Wheel=\"\" Emitter=\"\" Filter=\"\"\n"
        "                ModeMaster=\"\" ModeFrom=\"0/1\" ModeTo=\"0/1\">\n"
        "                <ChannelSet Name=\"\" DMXFrom=\"0/1\" PhysicalFrom=\"0\" PhysicalTo=\"1\"/>\n"
        "              </ChannelFunction>\n"
        "            </LogicalChannel>\n"
        "          </DMXChannel>\n"
        "\n"
        "          <!-- Ch 7: Green -->\n"
        "          <DMXChannel DMXBreak=\"1\" Offset=\"7\"\n"
        "            Default=\"0/1\" Highlight=\"255/1\" Geometry=\"Beam\">\n"
        "            <LogicalChannel Attribute=\"ColorAdd_G\" Snap=\"No\"\n"
        "              Master=\"Grand\" MibFade=\"0\" DMXChangeTimeLimit=\"0\">\n"
        "              <ChannelFunction Attribute=\"ColorAdd_G\" Name=\"Green\"\n"
        "                OriginalAttribute=\"\" DMXFrom=\"0/1\" DMXTo=\"255/1\"\n"
        "                PhysicalFrom=\"0\" PhysicalTo=\"1\"\n"
        "                RealFade=\"0\" RealAcceleration=\"0\"\n"
        "                Wheel=\"\" Emitter=\"\" Filter=\"\"\n"
        "                ModeMaster=\"\" ModeFrom=\"0/1\" ModeTo=\"0/1\">\n"
        "                <ChannelSet Name=\"\" DMXFrom=\"0/1\" PhysicalFrom=\"0\" PhysicalTo=\"1\"/>\n"
        "              </ChannelFunction>\n"
        "            </LogicalChannel>\n"
        "          </DMXChannel>\n"
        "\n"
        "          <!-- Ch 8: Blue -->\n"
        "          <DMXChannel DMXBreak=\"1\" Offset=\"8\"\n"
        "            Default=\"0/1\" Highlight=\"255/1\" Geometry=\"Beam\">\n"
        "            <LogicalChannel Attribute=\"ColorAdd_B\" Snap=\"No\"\n"
        "              Master=\"Grand\" MibFade=\"0\" DMXChangeTimeLimit=\"0\">\n"
        "              <ChannelFunction Attribute=\"ColorAdd_B\" Name=\"Blue\"\n"
        "                OriginalAttribute=\"\" DMXFrom=\"0/1\" DMXTo=\"255/1\"\n"
        "                PhysicalFrom=\"0\" PhysicalTo=\"1\"\n"
        "                RealFade=\"0\" RealAcceleration=\"0\"\n"
        "                Wheel=\"\" Emitter=\"\" Filter=\"\"\n"
        "                ModeMaster=\"\" ModeFrom=\"0/1\" ModeTo=\"0/1\">\n"
        "                <ChannelSet Name=\"\" DMXFrom=\"0/1\" PhysicalFrom=\"0\" PhysicalTo=\"1\"/>\n"
        "              </ChannelFunction>\n"
        "            </LogicalChannel>\n"
        "          </DMXChannel>\n"
        "\n"
        "          <!-- Ch 9: Strobe -->\n"
        "          <DMXChannel DMXBreak=\"1\" Offset=\"9\"\n"
        "            Default=\"0/1\" Highlight=\"0/1\" Geometry=\"Beam\">\n"
        "            <LogicalChannel Attribute=\"Shutter1Strobe\" Snap=\"No\"\n"
        "              Master=\"None\" MibFade=\"0\" DMXChangeTimeLimit=\"0\">\n"
        "              <ChannelFunction Attribute=\"Shutter1Strobe\" Name=\"Strobe\"\n"
        "                OriginalAttribute=\"\" DMXFrom=\"0/1\" DMXTo=\"255/1\"\n"
        "                PhysicalFrom=\"0\" PhysicalTo=\"25\"\n"
        "                RealFade=\"0\" RealAcceleration=\"0\"\n"
        "                Wheel=\"\" Emitter=\"\" Filter=\"\"\n"
        "                ModeMaster=\"\" ModeFrom=\"0/1\" ModeTo=\"0/1\">\n"
        "                <ChannelSet Name=\"Open\" DMXFrom=\"0/1\" PhysicalFrom=\"0\" PhysicalTo=\"0\"/>\n"
        "                <ChannelSet Name=\"Strobe\" DMXFrom=\"1/1\" PhysicalFrom=\"1\" PhysicalTo=\"25\"/>\n"
        "              </ChannelFunction>\n"
        "            </LogicalChannel>\n"
        "          </DMXChannel>\n"
        "\n"
        "          <!-- Ch 10: Gobo -->\n"
        "          <DMXChannel DMXBreak=\"1\" Offset=\"10\"\n"
        "            Default=\"0/1\" Highlight=\"0/1\" Geometry=\"Beam\">\n"
        "            <LogicalChannel Attribute=\"Gobo1\" Snap=\"Yes\"\n"
        "              Master=\"None\" MibFade=\"0\" DMXChangeTimeLimit=\"0\">\n"
        "              <ChannelFunction Attribute=\"Gobo1\" Name=\"Gobo\"\n"
        "                OriginalAttribute=\"\" DMXFrom=\"0/1\" DMXTo=\"255/1\"\n"
        "                PhysicalFrom=\"0\" PhysicalTo=\"255\"\n"
        "                RealFade=\"0\" RealAcceleration=\"0\"\n"
        "                Wheel=\"Gobo1\" Emitter=\"\" Filter=\"\"\n"
        "                ModeMaster=\"\" ModeFrom=\"0/1\" ModeTo=\"0/1\">\n"
        "                <ChannelSet Name=\"Open\" DMXFrom=\"0/1\" PhysicalFrom=\"0\" PhysicalTo=\"0\"/>\n"
        "              </ChannelFunction>\n"
        "            </LogicalChannel>\n"
        "          </DMXChannel>\n"
        "\n"
        "          <!-- Ch 11-64: Reserved -->\n"
        "          <!-- (54 reserved channels for future expansion) -->\n"
        "\n"
        "        </DMXChannels>\n"
        "\n"
        "        <!-- Revision history -->\n"
        "        <Revisions>\n"
        "          <Revision Text=\"Initial GDTF export\" Date=\"2026-05-13\" UserName=\"IDHMFIS\"/>\n"
        "        </Revisions>\n"
        "\n"
        "      </DMXMode>\n"
        "    </DMXModes>\n"
        "\n"
        "    <!-- Wheel definitions -->\n"
        "    <Wheels>\n"
        "      <Wheel Name=\"Gobo1\">\n"
        "        <Slot Name=\"Open\" Color=\"0.0,0.0,0.0\"/>\n"
        "        <Slot Name=\"Gobo1\"/>\n"
        "        <Slot Name=\"Gobo2\"/>\n"
        "        <Slot Name=\"Gobo3\"/>\n"
        "      </Wheel>\n"
        "    </Wheels>\n"
        "\n"
        "    <!-- Attribute definitions (GDTF built-in, referenced above) -->\n"
        "    <AttributeDefinitions>\n"
        "      <ActivationGroups/>\n"
        "      <FeatureGroups>\n"
        "        <FeatureGroup Name=\"Position\" Pretty=\"Pos\">\n"
        "          <Feature Name=\"PanTilt\"/>\n"
        "        </FeatureGroup>\n"
        "        <FeatureGroup Name=\"Dimmer\" Pretty=\"Dim\">\n"
        "          <Feature Name=\"Dimmer\"/>\n"
        "        </FeatureGroup>\n"
        "        <FeatureGroup Name=\"Color\" Pretty=\"Color\">\n"
        "          <Feature Name=\"RGB\"/>\n"
        "        </FeatureGroup>\n"
        "        <FeatureGroup Name=\"Beam\" Pretty=\"Beam\">\n"
        "          <Feature Name=\"Beam\"/>\n"
        "        </FeatureGroup>\n"
        "        <FeatureGroup Name=\"Gobo\" Pretty=\"Gobo\">\n"
        "          <Feature Name=\"Gobo\"/>\n"
        "        </FeatureGroup>\n"
        "      </FeatureGroups>\n"
        "      <Attributes>\n"
        "        <Attribute Name=\"Pan\"             Pretty=\"P\"   ActivationGroup=\"\" Feature=\"Position.PanTilt\" PhysicalUnit=\"Angle\"/>\n"
        "        <Attribute Name=\"Tilt\"            Pretty=\"T\"   ActivationGroup=\"\" Feature=\"Position.PanTilt\" PhysicalUnit=\"Angle\"/>\n"
        "        <Attribute Name=\"Dimmer\"          Pretty=\"Dim\" ActivationGroup=\"\" Feature=\"Dimmer.Dimmer\"    PhysicalUnit=\"LuminousIntensity\"/>\n"
        "        <Attribute Name=\"ColorAdd_R\"      Pretty=\"R\"   ActivationGroup=\"\" Feature=\"Color.RGB\"       PhysicalUnit=\"ColorComponent\"/>\n"
        "        <Attribute Name=\"ColorAdd_G\"      Pretty=\"G\"   ActivationGroup=\"\" Feature=\"Color.RGB\"       PhysicalUnit=\"ColorComponent\"/>\n"
        "        <Attribute Name=\"ColorAdd_B\"      Pretty=\"B\"   ActivationGroup=\"\" Feature=\"Color.RGB\"       PhysicalUnit=\"ColorComponent\"/>\n"
        "        <Attribute Name=\"Shutter1Strobe\"  Pretty=\"Stb\" ActivationGroup=\"\" Feature=\"Beam.Beam\"       PhysicalUnit=\"Frequency\"/>\n"
        "        <Attribute Name=\"Gobo1\"           Pretty=\"G1\"  ActivationGroup=\"\" Feature=\"Gobo.Gobo\"       PhysicalUnit=\"None\"/>\n"
        "      </Attributes>\n"
        "    </AttributeDefinitions>\n"
        "\n"
        "  </FixtureType>\n"
        "</GDTF>\n";

    std::snprintf(xml, sizeof(xml), "%s", tmpl);
    return xml;
}

// ─────────────────────────────────────────────────────────────────────────────
//  Minimal 1x1 white PNG (placeholder thumbnail)
//  This is a pre-computed valid 1x1 white PNG, 67 bytes.
//  Required by some GDTF importers that expect a thumbnail file.
// ─────────────────────────────────────────────────────────────────────────────
static const uint8_t kThumbnailPng[] = {
    0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A, // PNG signature
    0x00, 0x00, 0x00, 0x0D, 0x49, 0x48, 0x44, 0x52, // IHDR length + type
    0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, // width=1, height=1
    0x08, 0x02, 0x00, 0x00, 0x00, 0x90, 0x77, 0x53, // 8bpc RGB, no interlace
    0xDE, 0x00, 0x00, 0x00, 0x0C, 0x49, 0x44, 0x41, // CRC + IDAT length+type
    0x54, 0x08, 0xD7, 0x63, 0xF8, 0xCF, 0xC0, 0x00, // IDAT data (zlib)
    0x00, 0x00, 0x02, 0x00, 0x01, 0xE2, 0x21, 0xBC, // IDAT data + CRC
    0x33, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4E, // IEND length+type
    0x44, 0xAE, 0x42, 0x60, 0x82              // IEND CRC
};

static const size_t kThumbnailPngSize = sizeof(kThumbnailPng);

} // anonymous namespace

// ─────────────────────────────────────────────────────────────────────────────
//  export_gdtf — public API
// ─────────────────────────────────────────────────────────────────────────────
bool export_gdtf(const std::string& path) {
    const char* xml_str = build_fixture_xml();
    size_t xml_len = std::strlen(xml_str);

    // Build ZIP entries
    std::vector<ZipEntry> entries;

    // Entry 1: fixture_type.xml
    {
        ZipEntry e;
        e.name = "fixture_type.xml";
        e.data.assign(
            reinterpret_cast<const uint8_t*>(xml_str),
            reinterpret_cast<const uint8_t*>(xml_str) + xml_len);
        e.crc = crc32_update(0, e.data.data(), e.data.size());
        entries.push_back(std::move(e));
    }

    // Entry 2: thumbnail.png (placeholder)
    {
        ZipEntry e;
        e.name = "thumbnail.png";
        e.data.assign(kThumbnailPng, kThumbnailPng + kThumbnailPngSize);
        e.crc = crc32_update(0, e.data.data(), e.data.size());
        entries.push_back(std::move(e));
    }

    // Build ZIP
    std::vector<uint8_t> zip = build_zip(entries);

    // Write to disk
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) {
        log::error("GDTF export: cannot open '%s' for writing", path.c_str());
        return false;
    }

    size_t written = std::fwrite(zip.data(), 1, zip.size(), f);
    std::fclose(f);

    if (written != zip.size()) {
        log::error("GDTF export: write incomplete (%zu / %zu bytes)",
                   written, zip.size());
        return false;
    }

    log::info("GDTF export: wrote %zu bytes to '%s'", zip.size(), path.c_str());
    return true;
}

} // namespace idhmfis

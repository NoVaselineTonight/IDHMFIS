#pragma once
// gdtf_exporter.h — General Device Type Format (GDTF) DIN SPEC 15800:2022 exporter.
//
// Exports an IDHMFIS laser show fixture definition as a GDTF file.
// GDTF is a ZIP archive containing:
//   fixture_type.xml  — root fixture definition (required)
//   wheels/           — gobo/colour wheel images (optional)
//   models/           — 3D geometry files (optional)
//   description.xml   — alias for fixture_type.xml in some implementations
//
// IDHMFIS fixture channel map (64 channels):
//   Ch 1-2:    Pan    (16-bit, MSB first)
//   Ch 3-4:    Tilt   (16-bit, MSB first)
//   Ch 5:      Dimmer / Intensity
//   Ch 6:      Red
//   Ch 7:      Green
//   Ch 8:      Blue
//   Ch 9:      Strobe
//   Ch 10:     Gobo selector
//   Ch 11-64:  Reserved / Extended (future use)
//
// The ZIP is written using a self-contained minimal ZIP writer (no zlib dependency).
// The XML is standard ASCII; the thumbnail is a 1x1 PNG placeholder.
//
// Usage:
//   bool ok = idhmfis::export_gdtf("C:/Shows/IDHMFIS.gdtf");
//   if (!ok) { /* error */ }

#include <string>
#include <cstdint>

namespace idhmfis {

// Export a GDTF fixture definition to the given file path.
// The file extension should be .gdtf (which is a ZIP file).
// Returns true on success, false on any write error.
bool export_gdtf(const std::string& path);

} // namespace idhmfis

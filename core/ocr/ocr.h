#pragma once
#include <string>
#include <vector>
#include <cstdint>

// OCR wrapper around Windows.Media.Ocr (Win10+). Rather than pulling in a
// full C++/WinRT or Tesseract dependency -- both of which are painful in
// MinGW -- we spawn a short PowerShell script that talks to WinRT natively
// and returns a JSON result. Zero bundled binaries, zero WinRT boilerplate
// in the DLL. Cost is one process spawn per call.

namespace ocr {

struct OcrResult {
    bool        ok;
    std::string text;      // flat concatenated text
    std::string json;      // full result: lines + word bounding boxes
    std::string engine;    // "winrt"
    std::string error;
};

// Runs OCR on a PNG blob. `language` is an optional BCP-47 tag such as
// "en-US" or "fr-FR"; empty means "use user's installed OCR languages".
OcrResult Recognize(const std::vector<uint8_t>& pngBytes,
                    const std::string& language = "");

// Utility: base64 decode. Exposed so the HTTP route can reuse it without
// duplicating the decoder.
std::vector<uint8_t> Base64Decode(const std::string& s);

} // namespace ocr

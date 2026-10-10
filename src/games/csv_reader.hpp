#pragma once
#include <string>
#include <vector>
#include "error.hpp"

// A logical CSV record and its physical starting line for diagnostics.
struct CsvRecord
{
    // One-based logical record number, including the header.
    size_t number;
    // One-based physical line where this record starts.
    size_t line;
    // Decoded fields preserving internal text exactly.
    std::vector<std::string> fields;
};
// Parses UTF-8 RFC-style CSV, including BOM, CRLF, and multiline fields.
E<std::vector<CsvRecord>> readCsv(const std::string &text);

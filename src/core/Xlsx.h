// Montage — Office Open XML workbooks (XLSX, ECMA-376): written for lists people open in a spreadsheet (cue sheets,
// dialogue lists, CCSLs), and read back. Each sheet is rows of cells, text or numbers; the first row of a sheet
// written with a header is bold and frozen. Read: any workbook's sheets by name, cells by their column (shared
// strings, inline strings, numbers and booleans as written; formulas as their cached values).
#pragma once

#include <string>
#include <vector>

namespace montage {

struct XlsxCell {
    std::string text;     // a number's too, as written ("12.5")
    bool number = false;  // a numeric cell
    double value = 0;

    XlsxCell() = default;
    XlsxCell(std::string t) : text(std::move(t)) {}
    XlsxCell(const char* t) : text(t) {}
    static XlsxCell num(double v);
    bool operator==(const XlsxCell& o) const { return text == o.text && number == o.number; }
};

struct XlsxSheet {
    std::string name;  // up to 31 characters, none of []:*?/\ (fixed up when written)
    std::vector<std::vector<XlsxCell>> rows;
    bool header = true;          // the first row bold and frozen
    std::vector<double> widths;  // column widths in characters (0 or missing: the default)
};

// The workbook's bytes (a zip archive); at least one sheet is written (an empty one when there are none).
std::string writeXlsx(const std::vector<XlsxSheet>& sheets);
bool writeXlsxFile(const std::string& path, const std::vector<XlsxSheet>& sheets, std::string* error = nullptr);
// A workbook's sheets in order, each row to its last cell (missing cells empty); false with a reason if it is not
// one. `header` and `widths` are not read.
bool readXlsx(const std::string& bytes, std::vector<XlsxSheet>& out, std::string* error = nullptr);
bool readXlsxFile(const std::string& path, std::vector<XlsxSheet>& out, std::string* error = nullptr);

// "A" for column 0, "AA" for 26.
std::string xlsxColumn(int index);

}  // namespace montage

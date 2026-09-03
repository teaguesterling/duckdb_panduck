#include "rtf_reader.hpp"

#include "duck_block_types.hpp"
#include "duckdb/common/file_system.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/main/extension/extension_loader.hpp"

#include <cctype>
#include <map>
#include <cstdlib>

namespace duckdb {
namespace rtf {

namespace {

//! Encode a Unicode code point as UTF-8.
void AppendUtf8(std::string &out, int32_t cp) {
	if (cp < 0 || cp > 0x10FFFF) {
		return;
	}
	if (cp < 0x80) {
		out.push_back(static_cast<char>(cp));
	} else if (cp < 0x800) {
		out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
		out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
	} else if (cp < 0x10000) {
		out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
		out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
		out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
	} else {
		out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
		out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
		out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
		out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
	}
}

//! \'hh escapes are CP1252 in practice, which differs from Latin-1 only in 0x80-0x9F.
//! Outside that window the byte value is the code point.
int32_t Cp1252ToCodepoint(uint8_t byte) {
	static const int32_t HIGH[32] = {0x20AC, 0x0081, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021,
	                                 0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0x008D, 0x017D, 0x008F,
	                                 0x0090, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
	                                 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x009D, 0x017E, 0x0178};
	if (byte >= 0x80 && byte <= 0x9F) {
		return HIGH[byte - 0x80];
	}
	return static_cast<int32_t>(byte);
}

//! Destinations whose contents are document metadata, not body text. Their groups are
//! skipped wholesale. `\*` ignorable destinations (bookmarks, footnote separators,
//! generator stamps) are handled generically and need no entry here.
bool IsSkippedDestination(const std::string &word) {
	return word == "fonttbl" || word == "colortbl" || word == "info" || word == "pict" || word == "object" ||
	       word == "header" || word == "footer" || word == "footnote" || word == "listtable" ||
	       word == "listoverridetable" || word == "rsidtbl" || word == "generator" || word == "themedata" ||
	       word == "colorschememapping" || word == "latentstyles" || word == "datastore" || word == "xmlnstbl" ||
	       word == "upr" || word == "annotation";
}

struct CharFormat {
	bool bold = false;
	bool italic = false;
	bool underline = false;
	bool strike = false;

	bool Plain() const {
		return !bold && !italic && !underline && !strike;
	}
	bool operator!=(const CharFormat &o) const {
		return bold != o.bold || italic != o.italic || underline != o.underline || strike != o.strike;
	}
	//! duck_block's inline vocabulary is flat, so a run carrying several attributes is
	//! reported by its strongest one. Documented limitation of the basic reader; nested
	//! inline structure is a later refinement.
	std::string ElementType() const {
		if (bold) {
			return DuckBlockTypes::INLINE_BOLD;
		}
		if (italic) {
			return DuckBlockTypes::INLINE_ITALIC;
		}
		if (underline) {
			return DuckBlockTypes::INLINE_UNDERLINE;
		}
		if (strike) {
			return DuckBlockTypes::INLINE_STRIKETHROUGH;
		}
		return DuckBlockTypes::INLINE_TEXT;
	}
};

struct GroupState {
	CharFormat fmt;
	int uc = 1;           //!< how many fallback chars follow a \uN
	bool skip = false;    //!< inside an ignored destination
	bool style = false;   //!< inside {\stylesheet}
	bool in_info = false; //!< inside {\info}, where a few children ARE wanted
	std::string meta_key; //!< non-empty: text goes to metadata under this pandoc key
};

class RtfParser {
public:
	explicit RtfParser(const std::string &src) : src_(src) {
	}

	std::vector<RtfBlock> Parse() {
		stack_.push_back(GroupState());
		while (pos_ < src_.size()) {
			char c = src_[pos_];
			if (c == '{') {
				pos_++;
				stack_.push_back(stack_.back());
				fresh_ = true;
			} else if (c == '}') {
				pos_++;
				if (style_entry_open_) {
					FinishStyleEntry();
				}
				if (stack_.size() > 1) {
					stack_.pop_back();
				}
				fresh_ = false;
			} else if (c == '\\') {
				ReadControl();
			} else if (c == '\r' || c == '\n') {
				pos_++; // line breaks are formatting in the RTF source, not content
			} else {
				if (skip_chars_ > 0) {
					skip_chars_--; // ANSI fallback char following a \uN
				} else {
					AppendText(std::string(1, c));
				}
				pos_++;
				fresh_ = false;
			}
		}
		FlushParagraph();
		// AFTER the blocks -- spec 6.2 makes body-then-metadata a contract. Sorted, because
		// std::map iterates sorted and that is also pandoc's Meta serialisation order.
		for (auto &kv : meta_) {
			auto text = kv.second;
			// Trimmed here rather than via a helper: this TU has none, and RTF pads a
			// destination's text with the space that separates a control word from its
			// argument -- `{\title The Title}` arrives as " The Title".
			size_t b = text.find_first_not_of(" \t\r\n");
			size_t e = text.find_last_not_of(" \t\r\n");
			text = (b == std::string::npos) ? std::string() : text.substr(b, e - b + 1);
			RtfBlock block;
			block.kind = DuckBlockTypes::KIND_VALUE;
			block.element_type = DuckBlockTypes::VALUE_INLINES;
			block.key = kv.first;
			if (!text.empty()) {
				RtfInline run;
				run.element_type = DuckBlockTypes::INLINE_TEXT;
				run.content = text;
				block.inlines.push_back(std::move(run));
			}
			blocks_.push_back(std::move(block));
		}
		return std::move(blocks_);
	}

private:
	void ReadControl() {
		pos_++; // consume backslash
		if (pos_ >= src_.size()) {
			return;
		}
		char c = src_[pos_];

		if (!std::isalpha(static_cast<unsigned char>(c))) {
			// Control symbol: \\ \{ \} \* \'hh \~ \- etc.
			pos_++;
			if (c == '\'') {
				if (pos_ + 1 < src_.size()) {
					auto hex = src_.substr(pos_, 2);
					pos_ += 2;
					// A \'hh escape counts as ONE character against a pending \ucN skip.
					// It must be dropped whole: decoding first and trimming bytes
					// afterwards splits a multi-byte UTF-8 sequence, which is how
					// LibreOffice's "\uN \'hh" fallback pairs produced invalid UTF-8.
					if (skip_chars_ > 0) {
						skip_chars_--;
					} else {
						auto byte = static_cast<uint8_t>(std::strtol(hex.c_str(), nullptr, 16));
						std::string utf8;
						AppendUtf8(utf8, Cp1252ToCodepoint(byte));
						AppendText(utf8);
					}
				}
			} else if (c == '*') {
				// Ignorable destination -- drop this whole group.
				stack_.back().skip = true;
			} else if (c == '\\' || c == '{' || c == '}') {
				AppendText(std::string(1, c));
			} else if (c == '~') {
				AppendText("\xC2\xA0"); // non-breaking space
			}
			fresh_ = false;
			return;
		}

		// Control word: letters, then an optional signed integer parameter.
		size_t start = pos_;
		while (pos_ < src_.size() && std::isalpha(static_cast<unsigned char>(src_[pos_]))) {
			pos_++;
		}
		std::string word = src_.substr(start, pos_ - start);

		bool has_param = false;
		int param = 0;
		bool negative = false;
		if (pos_ < src_.size() && src_[pos_] == '-') {
			negative = true;
			pos_++;
		}
		size_t dstart = pos_;
		while (pos_ < src_.size() && std::isdigit(static_cast<unsigned char>(src_[pos_]))) {
			pos_++;
		}
		if (pos_ > dstart) {
			has_param = true;
			param = std::atoi(src_.substr(dstart, pos_ - dstart).c_str());
			if (negative) {
				param = -param;
			}
		}
		// A single space after a control word is a delimiter, not content.
		if (pos_ < src_.size() && src_[pos_] == ' ') {
			pos_++;
		}

		bool was_fresh = fresh_;
		fresh_ = false;
		ApplyControlWord(word, param, has_param, was_fresh);
	}

	void ApplyControlWord(const std::string &word, int param, bool has_param, bool was_fresh) {
		auto &g = stack_.back();

		// A destination keyword is only meaningful as the first token of its group.
		if (was_fresh) {
			if (word == "stylesheet") {
				g.style = true;
				return;
			}
			if (word == "info") {
				// {\info} IS SKIPPED BY DEFAULT AND SELECTIVELY OPENED. Most of its children
				// are timestamps -- \creatim, \revtim, \printim -- which pandoc does not
				// map to anything, so descending wholesale would put date fragments in the
				// document. Marking the group instead lets \title, \author and \subject
				// re-open it for themselves below.
				g.skip = true;
				g.in_info = true;
				return;
			}
			if (IsSkippedDestination(word)) {
				g.skip = true;
				return;
			}
		}

		// METADATA CAPTURE, and it must come before the g.skip early-return below: these
		// words arrive INSIDE a group already marked skip, and their whole job is to
		// re-open it.
		//
		// RTF's `author` is a single MetaInlines, NOT a list -- measured. LaTeX's \author
		// yields MetaList for the same logical field and Org concatenates repeated
		// #+AUTHOR: into one MetaInlines. Three formats, three arrangements, all pandoc's,
		// so no reader here may generalise its author handling from another's.
		if (g.in_info && (word == "title" || word == "author" || word == "subject")) {
			g.skip = false;
			g.meta_key = word;
			return;
		}
		if (word == "generator") {
			// NOT inside {\info}: `{\*\generator ...}` is a top-level ignorable
			// destination. Assuming RTF metadata lives in \info would have missed it, and
			// it is the only metadata the LibreOffice fixture actually carries.
			g.skip = false;
			g.meta_key = "generator";
			return;
		}
		if (g.style) {
			// Inside the stylesheet: capture "\sN ... Style Name;" entries.
			if (word == "s" && has_param) {
				style_entry_open_ = true;
				style_entry_id_ = param;
			}
			return;
		}
		if (g.skip) {
			return;
		}

		if (word == "par") {
			FlushParagraph();
		} else if (word == "pard") {
			style_id_ = -1;
			outline_level_ = -1;
			// \pard resets paragraph properties, INCLUDING list membership. Without this a
			// single list turns every following paragraph into a list item.
			list_id_ = 0;
			list_level_ = 0;
		} else if (word == "plain") {
			g.fmt = CharFormat();
		} else if (word == "b") {
			g.fmt.bold = !has_param || param != 0;
		} else if (word == "i") {
			g.fmt.italic = !has_param || param != 0;
		} else if (word == "ul") {
			g.fmt.underline = !has_param || param != 0;
		} else if (word == "ulnone") {
			g.fmt.underline = false;
		} else if (word == "strike") {
			g.fmt.strike = !has_param || param != 0;
		} else if (word == "s" && has_param) {
			style_id_ = param;
		} else if (word == "outlinelevel" && has_param) {
			outline_level_ = param;
		} else if (word == "ls" && has_param) {
			// \lsN names the list this paragraph belongs to; \ilvlN its depth. Measured
			// against pandoc, which makes a BulletList from exactly the paragraphs carrying
			// these, and nothing from the fixture that has neither.
			list_id_ = param;
		} else if (word == "ilvl" && has_param) {
			list_level_ = param;
		} else if (word == "listtext") {
			// {\listtext ...} is the RENDERED BULLET -- the glyph and its tab, written into
			// the file so a non-list-aware renderer still shows something. It is presentation,
			// not content, and including it puts a literal bullet character at the front of
			// every list item's text. Suppressed like any other non-content destination.
			g.skip = true;
		} else if (word == "uc" && has_param) {
			g.uc = param;
		} else if (word == "u" && has_param) {
			int32_t cp = param;
			if (cp < 0) {
				cp += 65536; // \uN is a signed 16-bit value
			}
			std::string utf8;
			AppendUtf8(utf8, cp);
			AppendText(utf8);
			skip_chars_ = stack_.back().uc; // drop the ANSI fallback that follows
		} else if (word == "tab") {
			AppendText("\t");
		} else if (word == "line") {
			AppendText("\n");
		} else if (word == "emdash") {
			AppendText("\xE2\x80\x94");
		} else if (word == "endash") {
			AppendText("\xE2\x80\x93");
		} else if (word == "bullet") {
			AppendText("\xE2\x80\xA2");
		} else if (word == "lquote" || word == "rquote") {
			AppendText(word == "lquote" ? "\xE2\x80\x98" : "\xE2\x80\x99");
		} else if (word == "ldblquote" || word == "rdblquote") {
			AppendText(word == "ldblquote" ? "\xE2\x80\x9C" : "\xE2\x80\x9D");
		}
	}

	void AppendText(const std::string &text) {
		auto &g = stack_.back();
		if (!g.meta_key.empty()) {
			meta_[g.meta_key] += text;
			return;
		}
		if (g.skip) {
			return;
		}
		if (g.style) {
			style_entry_name_ += text;
			return;
		}
		const std::string &t = text;
		if (t.empty()) {
			return;
		}
		if (runs_.empty() || runs_.back().fmt != g.fmt) {
			runs_.push_back(Run {g.fmt, t});
		} else {
			runs_.back().text += t;
		}
	}

	void FinishStyleEntry() {
		if (style_entry_open_) {
			auto name = style_entry_name_;
			auto semi = name.find(';');
			if (semi != std::string::npos) {
				name = name.substr(0, semi);
			}
			// Trim
			size_t b = name.find_first_not_of(" \t");
			size_t e = name.find_last_not_of(" \t");
			if (b != std::string::npos) {
				styles_[style_entry_id_] = name.substr(b, e - b + 1);
			}
			style_entry_open_ = false;
		}
		style_entry_name_.clear();
	}

	//! Heading level, or 0. \outlinelevel wins when present; otherwise the paragraph's
	//! style is resolved through the stylesheet and matched against "Heading N".
	int HeadingLevel() const {
		if (outline_level_ >= 0 && outline_level_ <= 8) {
			return outline_level_ + 1;
		}
		auto it = styles_.find(style_id_);
		if (it != styles_.end()) {
			const auto &name = it->second;
			if (name.size() >= 8) {
				std::string lower;
				for (char c : name) {
					lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
				}
				if (lower.compare(0, 7, DuckBlockTypes::TYPE_HEADING) == 0) {
					size_t i = 7;
					while (i < lower.size() && lower[i] == ' ') {
						i++;
					}
					if (i < lower.size() && std::isdigit(static_cast<unsigned char>(lower[i]))) {
						int level = lower[i] - '0';
						if (level >= 1 && level <= 6) {
							return level;
						}
					}
				}
			}
		}
		return 0;
	}

	void FlushParagraph() {
		std::string all;
		bool any_format = false;
		for (auto &r : runs_) {
			all += r.text;
			if (!r.fmt.Plain()) {
				any_format = true;
			}
		}
		// Trim -- RTF paragraphs routinely carry leading/trailing layout whitespace.
		size_t b = all.find_first_not_of(" \t\n");
		size_t e = all.find_last_not_of(" \t\n");
		if (b == std::string::npos) {
			runs_.clear();
			return; // whitespace-only paragraph
		}
		std::string trimmed = all.substr(b, e - b + 1);

		RtfBlock block;
		int level = HeadingLevel();
		// A heading is never a list item, even when the paragraph carries \ls -- RTF permits
		// the combination and no consumer expects a heading inside a list.
		int list_depth = (level > 0 || list_id_ == 0) ? 0 : list_level_ + 1;

		// Open and close `list` containers around the run of items, matching every other
		// reader's shape.
		while (open_lists_ > list_depth) {
			open_lists_--;
		}
		while (open_lists_ < list_depth) {
			RtfBlock l;
			l.element_type = DuckBlockTypes::TYPE_LIST;
			l.level = 2 * open_lists_ + 1;
			// RTF's orderedness lives in the \listtable's level definitions, which this
			// reader does not parse. BULLET is the safe default and the measured one: pandoc
			// reports BulletList for the only fixture here that has a list, and a wrong
			// `ordered=true` would be a claim the document does not support. Recorded as a
			// gap rather than guessed.
			l.list_type = DuckBlockTypes::LIST_TYPE_BULLET;
			blocks_.push_back(std::move(l));
			open_lists_++;
		}

		if (level > 0) {
			block.element_type = DuckBlockTypes::TYPE_HEADING;
			block.heading_level = level;
		} else if (list_depth > 0) {
			block.element_type = DuckBlockTypes::TYPE_LIST_ITEM;
			block.level = 2 * list_depth;
		} else {
			block.element_type = DuckBlockTypes::TYPE_PARAGRAPH;
		}

		// Character formatting inside a heading is presentational -- both pandoc and
		// LibreOffice bold heading text as part of the heading style itself. Emitting the
		// title as an inline child would leave content NULL, so a consumer building a
		// table of contents from `content` would see an empty heading. Headings therefore
		// always flatten.
		if (!any_format || level > 0) {
			// Text-only run flattens into content, matching the duck_block spec's
			// normalized simple case (one duck_block per paragraph, no children).
			block.content = trimmed;
		} else {
			for (auto &r : runs_) {
				std::string t = r.text;
				size_t rb = t.find_first_not_of(" \t\n");
				if (rb == std::string::npos) {
					continue;
				}
				size_t re = t.find_last_not_of(" \t\n");
				// Preserve interior spacing but drop pure-whitespace runs.
				block.inlines.push_back(RtfInline {r.fmt.ElementType(), t});
				(void)re;
			}
			if (block.inlines.empty()) {
				block.content = trimmed;
			}
		}
		blocks_.push_back(std::move(block));
		runs_.clear();
	}

	struct Run {
		CharFormat fmt;
		std::string text;
	};

	const std::string &src_;
	size_t pos_ = 0;
	bool fresh_ = false;
	int skip_chars_ = 0;

	std::vector<GroupState> stack_;
	std::vector<Run> runs_;
	std::vector<RtfBlock> blocks_;
	//! Captured {\info} fields and {\*\generator}, keyed by PANDOC's names.
	std::map<std::string, std::string> meta_;

	int style_id_ = -1;
	int outline_level_ = -1;
	int list_id_ = 0;    //!< \lsN -- 0 means this paragraph is not in a list
	int list_level_ = 0; //!< \ilvlN -- depth within that list
	int open_lists_ = 0; //!< how many `list` containers are currently open

	std::map<int, std::string> styles_;
	bool style_entry_open_ = false;
	int style_entry_id_ = 0;
	std::string style_entry_name_;
};

} // namespace

std::vector<RtfBlock> ParseRtfDocument(const std::string &data) {
	RtfParser parser(data);
	return parser.Parse();
}

} // namespace rtf

namespace {

//! One emitted row, already shaped like a duck_block.
struct BlockRow {
	std::string kind;
	std::string element_type;
	std::string content;
	bool has_level = false;
	int32_t level = 0;
	std::map<std::string, std::string> attributes;
	int32_t element_order = 0;
};

struct RtfReaderBindData : public TableFunctionData {
	std::vector<BlockRow> rows;
};

struct RtfReaderGlobalState : public GlobalTableFunctionState {
	idx_t offset = 0;

	static unique_ptr<GlobalTableFunctionState> Init(ClientContext &, TableFunctionInitInput &) {
		return make_uniq<RtfReaderGlobalState>();
	}
};

unique_ptr<FunctionData> RtfReaderBind(ClientContext &context, TableFunctionBindInput &input,
                                       vector<LogicalType> &return_types, vector<string> &names) {
	// Column order mirrors the duck_block struct so a row casts straight to duck_block.
	names = {"kind", "element_type", "content", "level", "encoding", "attributes", "element_order"};
	return_types = {LogicalType::VARCHAR, LogicalType::VARCHAR,
	                LogicalType::VARCHAR, LogicalType::INTEGER,
	                LogicalType::VARCHAR, LogicalType::MAP(LogicalType::VARCHAR, LogicalType::VARCHAR),
	                LogicalType::INTEGER};

	auto path = input.inputs[0].GetValue<string>();
	auto &fs = FileSystem::GetFileSystem(context);
	if (!fs.FileExists(path)) {
		throw IOException("read_rtf_blocks: file not found: %s", path);
	}
	auto handle = fs.OpenFile(path, FileOpenFlags::FILE_FLAGS_READ);
	auto size = fs.GetFileSize(*handle);
	std::string data;
	data.resize(size);
	if (size > 0) {
		fs.Read(*handle, const_cast<char *>(data.data()), size);
	}

	auto result = make_uniq<RtfReaderBindData>();
	int32_t order = 0;
	for (auto &block : rtf::ParseRtfDocument(data)) {
		BlockRow row;
		row.kind = block.kind.empty() ? DuckBlockTypes::KIND_BLOCK : block.kind;
		row.element_type = block.element_type;
		if (!block.key.empty()) {
			row.attributes[DuckBlockTypes::ATTR_KEY] = block.key;
		}
		row.content = block.content;
		row.element_order = order++;
		if (block.heading_level > 0) {
			row.attributes[DuckBlockTypes::ATTR_HEADING_LEVEL] = std::to_string(block.heading_level);
		}
		if (!block.list_type.empty()) {
			row.attributes[DuckBlockTypes::ATTR_LIST_TYPE] = block.list_type;
			row.attributes[DuckBlockTypes::ATTR_ORDERED_LEGACY] =
			    block.list_type == DuckBlockTypes::LIST_TYPE_ORDERED ? "true" : "false";
		}
		// EVERY ELEMENT CARRIES A STRUCTURAL LEVEL. Top level is 1; an inline is a CHILD
		// of its block, so it is one deeper. This reader emits no containers, so every
		// block sits at 1 and every inline at 2.
		const int32_t block_level = block.level > 0 ? block.level : 1;
		row.has_level = true;
		row.level = block_level;
		result->rows.push_back(std::move(row));

		for (auto &inl : block.inlines) {
			BlockRow child;
			child.kind = DuckBlockTypes::KIND_INLINE;
			child.element_type = inl.element_type;
			child.content = inl.content;
			child.has_level = true;
			child.level = block_level + 1;
			child.element_order = order++;
			result->rows.push_back(std::move(child));
		}
	}
	return std::move(result);
}

void RtfReaderScan(ClientContext &, TableFunctionInput &input, DataChunk &output) {
	auto &data = input.bind_data->Cast<RtfReaderBindData>();
	auto &state = input.global_state->Cast<RtfReaderGlobalState>();

	idx_t count = 0;
	while (state.offset < data.rows.size() && count < STANDARD_VECTOR_SIZE) {
		const auto &row = data.rows[state.offset];

		output.SetValue(0, count, Value(row.kind));
		output.SetValue(1, count, Value(row.element_type));
		// Empty content is NULL, per the duck_block convention for containers whose text
		// lives in structured inline children.
		output.SetValue(2, count, row.content.empty() ? Value(LogicalType::VARCHAR) : Value(row.content));
		output.SetValue(3, count, row.has_level ? Value::INTEGER(row.level) : Value(LogicalType::INTEGER));
		output.SetValue(4, count, Value(DuckBlockTypes::INLINE_TEXT));
		output.SetValue(5, count, DuckBlockTypes::CreateAttributesMap(row.attributes));
		output.SetValue(6, count, Value::INTEGER(row.element_order));

		state.offset++;
		count++;
	}
	output.SetCardinality(count);
}

} // namespace

void RegisterRtfReaderFunction(ExtensionLoader &loader) {
	TableFunction fn("read_rtf_blocks", {LogicalType::VARCHAR}, RtfReaderScan, RtfReaderBind,
	                 RtfReaderGlobalState::Init);
	loader.RegisterFunction(fn);
}

} // namespace duckdb

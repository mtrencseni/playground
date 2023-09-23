#include <algorithm>
#include <iostream>
#include <iterator>
#include <iomanip>
#include <sstream>
#include <fstream>
#include <locale>
#include <vector>
#include <memory>
#include <set>
#include <map>
#include <cstring>
#include <sys/stat.h> // for optimized file size

//
// coreutils implementation:
// https://github.com/coreutils/coreutils/blob/master/src/wc.c
//

class counter {
    public:
    virtual bool has_optimization() const { return false; }
    virtual void optimized_count(const std::string fname) { }
    virtual void process_wchar(const wchar_t, unsigned, bool) = 0;
    unsigned long get_count() const { return count; }
    void add(unsigned long i) { count += i; }
    void clear() { count = 0; }
    protected:
    unsigned long count = 0;
};

class byte_counter : public counter {
    public:
    bool has_optimization() const { return true; }
    virtual void optimized_count(const std::string fname) {
        struct stat st;
        stat(fname.c_str(), &st);
        count = st.st_size;
    }
    void process_wchar(const wchar_t, unsigned num_bytes, bool) {
        count += num_bytes;
    }
};

class char_counter : public counter {
    public:
    void process_wchar(const wchar_t, unsigned num_bytes, bool error) {
        if (MB_CUR_MAX == 1) {
            count += num_bytes;
        } else {
            if (error)
                return;
            count++;
        }
    }
};

class line_counter : public counter {
    public:
    void process_wchar(const wchar_t wc, unsigned num_bytes, bool error) {
        if (error)
            return;
        count += (wc == '\n');
    }
};

class longest_line_counter : public counter {
    public:
    void process_wchar(const wchar_t wc, unsigned, bool error) {
        if (error)
            return;
        if (wc == '\t') {
            current_length += 8 - (current_length % 8);
        } else {
            int width = wcwidth(wc);
            if (wc != '\n' && iswprint(wc) && width > 0)
                current_length += width;
            if (current_length > count)
                count = current_length;
            if (wc == '\n' || wc == '\r' || wc == '\f')
                current_length = 0;
        }
    }
    
    private:
    long current_length = 0;
};

bool is_word_sep(const wchar_t wc) {
    if (wc == '\n' || wc == '\r' || wc == '\f' || wc == '\v' || wc == ' ') {
        return true;
    }
    return static_cast<bool>(std::iswspace(wc));
}

class word_counter : public counter {
    public:
    void process_wchar(const wchar_t wc, unsigned, bool error) {
        if (error)
            return;
        bool whitespace = is_word_sep(wc);
        bool printable = iswprint(wc);
        if (in_word && whitespace) {
            in_word = false;
        } else if (!in_word && printable && !whitespace) {
            in_word = true;
            count++;
        }
    }
    
    private:
    bool in_word = false;
};

typedef std::vector< std::unique_ptr<counter> > counter_vector;

static inline bool is_basic (char c)
{
  switch (c)
    {
    case '\t': case '\v': case '\f':
    case ' ': case '!': case '"': case '#': case '%':
    case '&': case '\'': case '(': case ')': case '*':
    case '+': case ',': case '-': case '.': case '/':
    case '0': case '1': case '2': case '3': case '4':
    case '5': case '6': case '7': case '8': case '9':
    case ':': case ';': case '<': case '=': case '>':
    case '?':
    case 'A': case 'B': case 'C': case 'D': case 'E':
    case 'F': case 'G': case 'H': case 'I': case 'J':
    case 'K': case 'L': case 'M': case 'N': case 'O':
    case 'P': case 'Q': case 'R': case 'S': case 'T':
    case 'U': case 'V': case 'W': case 'X': case 'Y':
    case 'Z':
    case '[': case '\\': case ']': case '^': case '_':
    case 'a': case 'b': case 'c': case 'd': case 'e':
    case 'f': case 'g': case 'h': case 'i': case 'j':
    case 'k': case 'l': case 'm': case 'n': case 'o':
    case 'p': case 'q': case 'r': case 's': case 't':
    case 'u': case 'v': case 'w': case 'x': case 'y':
    case 'z': case '{': case '|': case '}': case '~':
      return 1;
    default:
      return 0;
    }
}

unsigned process_block(
    const char* cp, unsigned n, counter_vector& counters, bool eof) {
    long remaining = n;
    std::mbstate_t ps{};
    while (remaining > 0) {
        wchar_t wc;
        long num_bytes = 0;
        if (is_basic(*cp)) {
            wc = *cp;
            num_bytes = 1;
        } else {
            num_bytes = mbrtowc(&wc, cp, remaining, &ps);
        }
        bool error = false;
        if (num_bytes == 0) {
            // encountered null character
            num_bytes = 1;
        } else if (num_bytes == static_cast<std::size_t>(-1)) {
            // encountered bad wide character
            num_bytes = 1;
            error = true;
        } else if (num_bytes == static_cast<std::size_t>(-2)) {
            // encountered incomplete wide character, get more bytes if possible
            if (!eof)
                return remaining;
            num_bytes = remaining;
            error = true;
        } else {
            // success, read a wchar_t
        }
        std::for_each(counters.begin(), counters.end(),
            [&](auto& c) { c->process_wchar(wc, num_bytes, error); });
        cp += num_bytes;
        remaining -= num_bytes;
    }
    return remaining;
}

void process_stream(std::istream* is, counter_vector& counters) {
    const int block_read = 128*1024;
    char buf[block_read+sizeof(wchar_t)];
    int n_remaining = 0;
    while (is->peek() != EOF) {
        // there are bytes to be read
        is->read(buf + n_remaining, block_read);
        int n_read = is->gcount();
        int n_available = n_remaining + n_read;
        if (n_available < 1)
            return;
        n_remaining = process_block(
            buf, n_available, counters, is->peek() == EOF);
        // assert(n_remaining >= 0);
        // assert(n_remaining <= sizeof(wchar_t));
        // keep unprocessed part
        if (n_remaining > 0) {
            std::memcpy(buf, buf + n_available - n_remaining, n_remaining);
        }
    }
}

struct cl_args_t {
    std::set<std::string> flags;
    std::map<std::string, std::string> key_values;
    std::vector<std::string> filename_args;
};

cl_args_t get_cl_args(int argc, char** argv) {
    cl_args_t cl_args;
    for(auto i = 1; i < argc; i++) {
        auto arg = std::string(argv[i]);
        if (arg[0] == '-') {
            if (cl_args.filename_args.size() > 0) {
                std::cerr << "error: switches must precede filename arguments"
                << std::endl;
            }
            auto eqi = arg.find('=');
            if (eqi == std::string::npos) {
                // no '=' in arg
                cl_args.flags.insert(arg);
            } else {
                auto k = arg.substr(0, eqi);
                auto v = arg.substr(eqi+1, arg.length());
                cl_args.key_values[k] = v;
            }
        } else {
            cl_args.filename_args.push_back(arg); 
        }
    }
    return cl_args;
}

void normalize_flags(cl_args_t& cl_args) {
    // convert eg. -lm to -l -m
    std::set<std::string> new_flags;
    for (auto f : cl_args.flags) {
        if (f[0] == '-' && f[1] != '-' && f.length() > 2) {
            for (auto i = 1; i < f.length(); i++) {
                std::string new_f = std::string("-") + f[i];
                new_flags.insert(new_f);
            }
        } else {
            new_flags.insert(f);
        }
    }
    cl_args.flags = new_flags;
}

void check_bad_args(const cl_args_t& cl_args) {
    std::set<std::string> accepted_flags = {
        "-l", "--lines",
        "-w", "--words",
        "-m", "--chars",
        "-c", "--bytes",
        "-L", "--max-line-length",
              "--help",
              "--version"
    };
    for (auto f : cl_args.flags) {
        if (!accepted_flags.count(f)) {
            std::cerr << "invalid option: " << f << std::endl;
            exit(1);
        }
    }
    for (auto k : cl_args.key_values) {
        std::string must_be = "--files0-from";
        if (k.first.substr(0, must_be.length()) != must_be) {
            std::cerr << "invalid option: " << k.first << std::endl;
            exit(1);
        }
    }
}

void check_version_and_help(const cl_args_t& cl_args) {
    if (cl_args.flags.count("--help") > 0) {
        std::cout
        << "Usage: wc [OPTION]... [FILE]..." << std::endl
        << "  or:  wc [OPTION]... --files0-from=F" << std::endl
        << "Print newline, word, and byte counts for each FILE, and a total line if" << std::endl
        << "more than one FILE is specified.  A word is a non-zero-length sequence of" << std::endl
        << "characters delimited by white space." << std::endl
        << std::endl
        << "With no FILE, or when FILE is -, read standard input." << std::endl
        << std::endl
        << "The options below may be used to select which counts are printed, always in" << std::endl
        << "the following order: newline, word, character, byte, maximum line length." << std::endl
        << "  -c, --bytes            print the byte counts" << std::endl
        << "  -m, --chars            print the character counts" << std::endl
        << "  -l, --lines            print the newline counts" << std::endl
        << "      --files0-from=F    read input from the files specified by" << std::endl
        << "                           NUL-terminated names in file F;" << std::endl
        << "                           If F is - then read names from standard input" << std::endl
        << "  -L, --max-line-length  print the maximum display width" << std::endl
        << "  -w, --words            print the word counts" << std::endl
        << "      --help     display this help and exit" << std::endl
        << "      --version  output version information and exit" << std::endl;
        exit(0);
    }
    if (cl_args.flags.count("--version") > 0) {
        std::cout << "wc by Marton Trencseni (mtrencseni@gmail.com)"
            << std::endl;
        std::cout << "Source at https://github.com/mtrencseni/playground/wcpp.cpp"
            << std::endl;
        exit(0);
    }
}

counter_vector counters_from_arguments(const cl_args_t& cl_args) {
    counter_vector counters;
    if (cl_args.flags.size() == 0) {
        // these 3, in this order, are counted by gnu wc by default
        counters.push_back(std::make_unique<line_counter>());
        counters.push_back(std::make_unique<word_counter>());
        counters.push_back(std::make_unique<byte_counter>());
    } else {
        // this is the gnu order if arguments are specified
    if (cl_args.flags.count("-l") or cl_args.flags.count("--lines"))
        counters.push_back(std::make_unique<line_counter>());
    if (cl_args.flags.count("-w") or cl_args.flags.count("--words"))
        counters.push_back(std::make_unique<word_counter>());
    if (cl_args.flags.count("-m") or cl_args.flags.count("--chars"))
        counters.push_back(std::make_unique<char_counter>());
    if (cl_args.flags.count("-c") or cl_args.flags.count("--bytes"))
        counters.push_back(std::make_unique<byte_counter>());
    if (cl_args.flags.count("-L") or cl_args.flags.count("--max-line-length"))
        counters.push_back(std::make_unique<longest_line_counter>());
    }
    return counters;
}

typedef std::vector< std::string > row_t;
typedef std::vector< row_t       > table_t;

row_t tabulate(const counter_vector& counters, std::string suffix = "") {
    row_t row;
    std::for_each(counters.begin(), counters.end(),
        [&](auto& counter) {
            row.push_back(std::to_string(counter->get_count()));
        });
    if (suffix.length())
        row.push_back(suffix);
    return row;
}

unsigned long get_max_width(const table_t& table, bool skip_last=false) {
    unsigned long max_width = 0;
    std::for_each(table.begin(), table.end(),
        [&](auto& row) {
            auto last = row.end();
            if (skip_last)
                last--;
            std::for_each(row.begin(), last,
                [&](auto& s) {
                    max_width = std::max(max_width, s.length());
                });
        });
    return max_width;
}

void print_table(const table_t& table, std::ostream& os, bool left_justify_last=false) {
    if (table.size() == 1 && table[0].size() == 1) {
        // we're printing just one thing, like the output from `wc -l`
        os << table[0][0] << std::endl;
        return;
    }
    auto w = get_max_width(table, left_justify_last) + 1;
    std::for_each(table.begin(), table.end(),
        [&](auto&  row) {
            auto last = row.end();
            if (left_justify_last)
                last--;
            std::for_each(row.begin(), last,
                [&](auto& s) {
                    os << std::setfill(' ') << std::setw(w) << s;
                });
            if (left_justify_last)
                os << " " << *(--row.end());
            os << std::endl;
        });
}

void read_files0_from(cl_args_t& cl_args) {
    std::istream* is;
    std::ifstream f;
    if (cl_args.key_values["--files0-from"] == "-") {
        is = &std::cin;
    } else {
        auto filename = cl_args.key_values["--files0-from"];
        f.open(filename);
        if (!f.is_open()) {
            std::cerr << "cannot read from file: " << filename << std::endl;
            exit(1);
        }
        is = &f;
    }
    for (std::string filename; std::getline(*is, filename, '\0');)
        cl_args.filename_args.push_back(filename);
}

table_t process_stdin(const cl_args_t& cl_args) {
    table_t table;
    auto counters = counters_from_arguments(cl_args);
    process_stream(&std::cin, counters);
    table.push_back(tabulate(counters));
    return table;
}

counter_vector process_file(const std::string fname, const cl_args_t& cl_args) {
    auto file_counters = counters_from_arguments(cl_args);
    auto all_optimized = std::all_of(
        file_counters.begin(),
        file_counters.end(),
        [](auto& c) { return c->has_optimization(); }
        );
    if (all_optimized) {
        std::for_each(file_counters.begin(), file_counters.end(),
            [&](auto& c) { c->optimized_count(fname); });
    } else {
        std::ifstream f(fname);
        if (f.is_open()) {
            process_stream(&f, file_counters);
        } else {
            std::cerr << "cannot read from file: " << fname << std::endl;
        }
    }
    return file_counters;
}

table_t process_files(const cl_args_t& cl_args) {
    table_t table;
    auto total_counters = counters_from_arguments(cl_args);
    std::for_each(cl_args.filename_args.begin(), cl_args.filename_args.end(),
    [&](auto& fname) {
        auto file_counters = process_file(fname, cl_args);
        table.push_back(tabulate(file_counters, fname));
        for (auto i = 0; i < total_counters.size(); i++)
            total_counters[i]->add(file_counters[i]->get_count());
    });
    if (cl_args.filename_args.size() > 1)
        table.push_back(tabulate(total_counters, "total"));
    return table;
}

int main(int argc, char** argv) {
    setlocale(LC_ALL, "");
    auto cl_args = get_cl_args(argc, argv);
    check_version_and_help(cl_args);
    normalize_flags(cl_args);
    check_bad_args(cl_args);
    if (cl_args.key_values.count("--files0-from")) {
        read_files0_from(cl_args);
    }
    table_t table;
    bool left_justify_last;
    if (cl_args.filename_args.size() == 0) {
        table = process_stdin(cl_args);
        left_justify_last = false;
    } else {
        table = process_files(cl_args);
        left_justify_last = true;
    }
    print_table(table, std::cout, left_justify_last);
}

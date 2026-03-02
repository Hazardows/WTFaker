#include <chrono>
#include <thread>
#include <atomic>
#include <fstream>
#include <vector>
#include <sstream>

#include <windows.h>
#include <psapi.h>
#include <conio.h>

#pragma comment(lib, "psapi.lib")
#pragma comment(lib, "ntdll.lib")

using namespace std;

using u8 = unsigned char;
using u32 = unsigned int;
using u64 = unsigned long long;
using f64 = double;
using f32 = float;

namespace util {
    template <typename T>
    inline string to_string(T value) {
        ostringstream oss;
        oss << value;
        return oss.str();
    }
}

f64 get_absolute_time();
static bool file_copy(string source, string dest);
static bool file_exists(string file);
string get_directory_from_path(const string& filepath);
string get_current_directory();

void console_write(string message, u8 colour) {
    HANDLE consoleHandle = GetStdHandle(STD_OUTPUT_HANDLE);

    //CYAN, RED, GREEN, WHITE, YELLOW, MAGENTA
    static u8 levels[6] = { 3, 4, 2, 8, 6, 5 };
    SetConsoleTextAttribute(consoleHandle, levels[colour]);
    OutputDebugStringA(message.c_str());
    u64 length = message.size();
    DWORD numberWritten = 0;

    static bool got_console_buffer = false;
    static CONSOLE_SCREEN_BUFFER_INFO csbi;
    static HANDLE console_handle;
    if (!got_console_buffer) {
        console_handle = GetStdHandle(STD_OUTPUT_HANDLE);
        GetConsoleScreenBufferInfo(console_handle, &csbi);
        got_console_buffer = true;
    }
    WriteConsoleA(console_handle, message.c_str(), (DWORD)length, &numberWritten, 0);
    SetConsoleTextAttribute(consoleHandle, csbi.wAttributes);
}

enum color {
    CYAN = 0,
    RED = 1,
    GREEN = 2,
    WHITE = 3,
    YELLOW = 4,
    MAGENTA = 5
};

void log_output(color c, string log) {
    console_write(log + "\n", c);
}

static bool file_exists(string file) {
    DWORD atributes = GetFileAttributesA(file.c_str());
    if (atributes == INVALID_FILE_ATTRIBUTES) {
        return false;
    }
    return true;
}

class hclock {
private:
    f64 start_time = 0.0f;
    f64 elapsed_time = 0.0f;
    bool active = false;
public:
    hclock() : start_time(0), elapsed_time(0), active(false) {}

    void start() {
        start_time = get_absolute_time();
        elapsed_time = 0.0f;
        active = true;
    }

    void stop() {
        if (active) {
            elapsed_time = get_absolute_time() - start_time;
            active = false;
        }
    }

    void update() {
        if (active) {
            elapsed_time = get_absolute_time() - start_time;
        }
    }

    f64 elapsed() const {
        return elapsed_time;
    }
};

class executer {
private:
    atomic <bool> running{ false };
    atomic <bool> stop_requested{ false };
    atomic <bool> timeLimitExceeded{ false };
    atomic <bool> memoryLimitExceeded{ false };
    atomic <bool> runtimeError{ false };
    thread monitor_thread;
    HANDLE hProcess = NULL;
    SIZE_T peakMemory = 0;
    SIZE_T memoryLimit = 0;
    u32 timeLimit = 0;
    hclock timer;
    HANDLE hChildStd_IN_Rd = NULL;
    HANDLE hChildStd_IN_Wr = NULL;
    HANDLE hChildStd_OUT_Rd = NULL;
    HANDLE hChildStd_OUT_Wr = NULL;
    string output = "";
    thread output_reader_thread;
    DWORD exitCode = 0;
    DWORD processId = 0;
    atomic<bool> process_terminated{ false };
    chrono::high_resolution_clock::time_point processCreationTime;
    wstring workingDirectory;
    bool use_files_mode;

public:
    executer(bool files_mode = false) : use_files_mode(files_mode) {}

    ~executer() {
        stop();
    }

    void set_limits(u32 max_time_ms, SIZE_T max_memory_bytes) {
        timeLimit = max_time_ms;
        memoryLimit = max_memory_bytes;
    }

    void set_working_directory(const wstring dir) {
        workingDirectory = dir;
    }

    void cleanup() {
        if (hChildStd_IN_Rd) CloseHandle(hChildStd_IN_Rd);
        if (hChildStd_IN_Wr) CloseHandle(hChildStd_IN_Wr);
        if (hChildStd_OUT_Rd) CloseHandle(hChildStd_OUT_Rd);
        if (hChildStd_OUT_Wr) CloseHandle(hChildStd_OUT_Wr);

        if (hProcess) {
            CloseHandle(hProcess);
            hProcess = NULL;
        }
    }

    void stop() {
        stop_requested = true;
        running = false;

        if (monitor_thread.joinable()) {
            monitor_thread.join();
        }
        if (output_reader_thread.joinable()) {
            output_reader_thread.join();
        }

        cleanup();
    }

    bool run_files_mode(const string& cmd, const string& input_file, const string& output_file) {
        STARTUPINFOW si = { sizeof(si) };
        PROCESS_INFORMATION pi;

        reset_state();

        si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
        si.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
        si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
        si.dwFlags |= STARTF_USESTDHANDLES;

        string full_output_path = output_file;
        remove(full_output_path.c_str());

        for (int i = 0; i < 3; i++) {
            if (!file_exists(full_output_path)) break;
            remove(full_output_path.c_str());
            this_thread::sleep_for(chrono::milliseconds(10));
        }

        LPWSTR workingDirPtr = NULL;
        if (!workingDirectory.empty()) {
            workingDirPtr = const_cast<LPWSTR>(workingDirectory.c_str());
        }

        if (!CreateProcessW(NULL, const_cast<LPWSTR>(wstring(cmd.begin(), cmd.end()).c_str()),
            NULL, NULL, TRUE, CREATE_NEW_PROCESS_GROUP, NULL, workingDirPtr, &si, &pi)) {
            return false;
        }

        hProcess = pi.hProcess;
        processId = pi.dwProcessId;
        CloseHandle(pi.hThread);

        timer.start();
        processCreationTime = chrono::high_resolution_clock::now();
        running = true;
        monitor_thread = thread([this]() { this->monitorProcess(); });

        return true;
    }

    bool run_stdio_mode(const string& cmd, const string& input) {
        STARTUPINFOW si = { sizeof(si) };
        PROCESS_INFORMATION pi;
        SECURITY_ATTRIBUTES saAttr = { sizeof(SECURITY_ATTRIBUTES) };

        reset_state();

        saAttr.bInheritHandle = TRUE;
        saAttr.lpSecurityDescriptor = NULL;

        if (!CreatePipe(&hChildStd_IN_Rd, &hChildStd_IN_Wr, &saAttr, 0)) {
            return false;
        }
        if (!SetHandleInformation(hChildStd_IN_Wr, HANDLE_FLAG_INHERIT, 0)) {
            return false;
        }

        if (!CreatePipe(&hChildStd_OUT_Rd, &hChildStd_OUT_Wr, &saAttr, 0)) {
            return false;
        }
        if (!SetHandleInformation(hChildStd_OUT_Rd, HANDLE_FLAG_INHERIT, 0)) {
            return false;
        }

        si.hStdInput = hChildStd_IN_Rd;
        si.hStdOutput = hChildStd_OUT_Wr;
        si.hStdError = hChildStd_OUT_Wr;
        si.dwFlags |= STARTF_USESTDHANDLES;

        LPWSTR workingDirPtr = NULL;
        if (!workingDirectory.empty()) {
            workingDirPtr = const_cast<LPWSTR>(workingDirectory.c_str());
        }

        if (!CreateProcessW(NULL, const_cast<LPWSTR>(wstring(cmd.begin(), cmd.end()).c_str()),
            NULL, NULL, TRUE, CREATE_NEW_PROCESS_GROUP, NULL, workingDirPtr, &si, &pi)) {
            return false;
        }

        hProcess = pi.hProcess;
        processId = pi.dwProcessId;
        CloseHandle(pi.hThread);

        CloseHandle(hChildStd_IN_Rd);
        CloseHandle(hChildStd_OUT_Wr);

        if (!input.empty()) {
            DWORD bytesWritten;
            WriteFile(hChildStd_IN_Wr, input.c_str(), (DWORD)input.size(), &bytesWritten, NULL);
        }
        CloseHandle(hChildStd_IN_Wr);
        hChildStd_IN_Wr = NULL;

        timer.start();
        processCreationTime = chrono::high_resolution_clock::now();
        running = true;

        output_reader_thread = thread([this]() { this->readOutput(); });
        monitor_thread = thread([this]() { this->monitorProcess(); });

        return true;
    }

    bool run(const string& cmd, const string& input, bool capture_output = false) {
        return (use_files_mode ? run_files_mode(cmd, "time.in", "time.out") : run_stdio_mode(cmd, input));
    }

private:
    void reset_state() {
        timeLimitExceeded = false;
        memoryLimitExceeded = false;
        runtimeError = false;
        exitCode = 0;
        processId = 0;
        process_terminated = false;
        peakMemory = 0;
        output = "";

        cleanup();
    }

    void terminateProcess() {
        if (hProcess && running) {
            stop_requested = true;
            running = false;
            process_terminated = true;

            if (processId > 0) {
                GenerateConsoleCtrlEvent(CTRL_C_EVENT, processId);
                this_thread::sleep_for(chrono::milliseconds(10));
            }

            DWORD currentExitCode;
            if (GetExitCodeProcess(hProcess, &currentExitCode) && currentExitCode == STILL_ACTIVE) {
                TerminateProcess(hProcess, 0);
                WaitForSingleObject(hProcess, 50);
            }
        }
    }

public:
    void wait() {
        if (!hProcess) {
            return;
        }

        DWORD waitResult;

        if (timeLimit > 0) {
            auto currentTime = chrono::high_resolution_clock::now();
            auto overheadTime = chrono::duration_cast<chrono::milliseconds>(currentTime - processCreationTime).count();

            DWORD remainingTime = 0;
            if (timeLimit > overheadTime) {
                remainingTime = timeLimit - overheadTime;
            }

            if (remainingTime <= 0) {
                timeLimitExceeded = true;
                terminateProcess();
                WaitForSingleObject(hProcess, 50);
            }
            else {
                waitResult = WaitForSingleObject(hProcess, remainingTime);

                if (waitResult == WAIT_TIMEOUT) {
                    timeLimitExceeded = true;
                    terminateProcess();
                    WaitForSingleObject(hProcess, 50);
                }
                else if (waitResult == WAIT_OBJECT_0) {
                    GetExitCodeProcess(hProcess, &exitCode);
                    if (exitCode != 0) {
                        runtimeError = true;
                    }
                }
            }
        }
        else {
            WaitForSingleObject(hProcess, INFINITE);
            GetExitCodeProcess(hProcess, &exitCode);
            if (exitCode != 0) {
                runtimeError = true;
            }
        }

        timer.stop();
        stop_requested = true;
        running = false;

        if (monitor_thread.joinable()) {
            monitor_thread.join();
        }
        if (output_reader_thread.joinable()) {
            output_reader_thread.join();
        }

        CloseHandle(hProcess);
        hProcess = NULL;
    }

private:
    void monitorProcess() {
        while (running && !stop_requested) {
            SIZE_T currentMemory = getCurrentMemoryUsage();

            if (currentMemory > peakMemory) {
                peakMemory = currentMemory;
            }

            if (memoryLimit > 0 && currentMemory > memoryLimit) {
                memoryLimitExceeded = true;
                TerminateProcess(hProcess, 0);
                WaitForSingleObject(hProcess, 50);
                break;
            }

            DWORD currentExitCode;
            if (GetExitCodeProcess(hProcess, &currentExitCode)) {
                if (currentExitCode != STILL_ACTIVE) {
                    exitCode = currentExitCode;
                    if (currentExitCode != 0) {
                        runtimeError = true;
                    }
                    running = false;
                    break;
                }
            }

            this_thread::sleep_for(chrono::milliseconds(10));
        }
    }

    SIZE_T getCurrentMemoryUsage() {
        SIZE_T memoryUsage = 0;

        if (hProcess) {
            PROCESS_MEMORY_COUNTERS pmc;
            if (GetProcessMemoryInfo(hProcess, &pmc, sizeof(pmc))) {
                memoryUsage = pmc.WorkingSetSize;
            }
            else {
                SIZE_T minWS, maxWS;
                if (GetProcessWorkingSetSize(hProcess, &minWS, &maxWS)) {
                    memoryUsage = maxWS;
                }
            }
        }

        return memoryUsage;
    }

    void readOutput() {
        DWORD bytesRead;
        CHAR buffer[4096];
        string result;

        while (running && !stop_requested) {
            if (!ReadFile(hChildStd_OUT_Rd, buffer, sizeof(buffer) - 1, &bytesRead, NULL) || bytesRead == 0) {
                break;
            }
            buffer[bytesRead] = '\0';
            result += buffer;
        }

        output = result;
        CloseHandle(hChildStd_OUT_Rd);
        hChildStd_OUT_Rd = NULL;
    }

public:
    u64 getPeakMemory() const {
        return peakMemory;
    }

    u32 getExecutionTime() const {
        return static_cast<u32>(timer.elapsed() * 1000.0f);
    }

    string get_output() const {
        return output;
    }

    bool was_tle() const {
        return timeLimitExceeded;
    }

    bool was_mle() const {
        return memoryLimitExceeded;
    }

    bool was_rte() const {
        return runtimeError;
    }

    DWORD get_exit_code() const {
        return exitCode;
    }

    bool is_files_mode() const {
        return use_files_mode;
    }
};

class text_loader {
private:
    string file;
    string contents = "";

public:
    text_loader(string filename) : file(filename) {}

    void load() {
        fstream in(file);
        string s;
        while (getline(in, s)) {
            contents += s + "\n";
        }
    }

    string get_loaded_text() const {
        return contents;
    }
};

static bool file_copy(string source, string dest) {
        BOOL result = CopyFileA(source.c_str(), dest.c_str(), false);
        this_thread::sleep_for(chrono::milliseconds(10));
        if (result && file_exists(dest)) {
            return true;
        }
    return false;
}

f64 get_absolute_time() {
    static f64 clock_frequency = 0.0f;
    static LARGE_INTEGER start_time;

    if (!clock_frequency) {
        LARGE_INTEGER frequency;
        QueryPerformanceFrequency(&frequency);
        clock_frequency = 1.0 / (f64)frequency.QuadPart;
        QueryPerformanceCounter(&start_time);
    }

    LARGE_INTEGER now_time;
    QueryPerformanceCounter(&now_time);
    return (f64)now_time.QuadPart * clock_frequency;
}

bool test_passed(string expected_file, string actual_file_or_output, bool files_mode = false) {
    text_loader expected_loader(expected_file);
    expected_loader.load();
    string expected = expected_loader.get_loaded_text();

    string actual;

    if (files_mode) {
        if (!file_exists(actual_file_or_output)) {
            return false;
        }

        text_loader actual_loader(actual_file_or_output);
        actual_loader.load();
        actual = actual_loader.get_loaded_text();
    }
    else {
        actual = actual_file_or_output;
    }

    istringstream estream(expected), astream(actual);
    // string expected_line, actual_line;
    string expected_token, actual_token;

    /*while (true) {
        expected_line = "";
        actual_line = "";
        getline(estream, expected_line);
        getline(astream, actual_line);
        istringstream expected_stream(expected_line), actual_stream(actual_line);

        if (expected_line == "" || actual_line == "") {
            break;
        }
        
        while (expected_stream >> expected_token && actual_stream >> actual_token) {
            if (expected_token != actual_token) {
                return false;
            }
        }
    }*/

    while (estream >> expected_token && astream >> actual_token) {
        if (expected_token != actual_token) {
            return false;
        }
    }

    expected_token = "";
    actual_token = "";
    estream >> expected_token;
    astream >> actual_token;

    return !(estream >> expected_token) && !(astream >> actual_token);
}

enum file_in_out_format {
    FILE_NUM_IN,
    NUM_IN,
    FILE_IN_NUM,
    FILE_IN_AUTO
};

enum case_enum_format {
    F_0X,
    F_X,
    F_X_AUTO
};

struct config {
    u32 time_limit;
    u32 mem_limit;
    file_in_out_format io_format;
    case_enum_format ce_format;
    bool use_stdio;
    u32 first_case;
    string base_name;
    string cpp_compiler;
    string cpp_standard;
};

static string format_filename(string base_name, u32 num, file_in_out_format fformat, case_enum_format cformat, bool is_in) {
    string formatted_num = util::to_string(num);
    if (cformat == F_0X && num < 10) {
        formatted_num = "0" + formatted_num;
    }

    switch (fformat) {
    case NUM_IN:
        return formatted_num + (is_in ? ".in" : ".out");
    case FILE_IN_NUM:
        return base_name + (is_in ? ".in." : ".out.") + formatted_num;
    case FILE_NUM_IN:
    default:
        return base_name + "." + formatted_num + (is_in ? ".in" : ".out");
    }
}

bool set_working_directory(wstring path) {
    if (SetCurrentDirectoryW(path.c_str())) {
        return true;
    }
    else {
        DWORD error = GetLastError();
        log_output(RED, "Error al cambiar directorio: " + util::to_string(error));
        return false;
    }
}

string extract_base_name_from_wide(const wstring& wide_path) {
    size_t last_slash = wide_path.find_last_of(L"\\/");
    wstring wide_filename;

    if (last_slash != wstring::npos) {
        wide_filename = wide_path.substr(last_slash + 1);
    }
    else {
        wide_filename = wide_path;
    }

    size_t last_dot = wide_filename.find_last_of(L'.');
    if (last_dot != wstring::npos) {
        wide_filename = wide_filename.substr(0, last_dot);
    }

    return string(wide_filename.begin(), wide_filename.end());
}

string get_program_directory() {
    wchar_t buffer[MAX_PATH];
    GetModuleFileNameW(NULL, buffer, MAX_PATH);

    wstring path(buffer);
    size_t last_slash = path.find_last_of(L"\\/");
    if (last_slash != wstring::npos) {
        path = path.substr(0, last_slash);
    }

    return string(path.begin(), path.end());
}

void load_config(string filename, config& c) {
    c.time_limit = 1000;
    c.mem_limit = 64000000;
    c.io_format = FILE_IN_AUTO;
    c.ce_format = F_X_AUTO;
    c.first_case = 1;
    c.use_stdio = false;
    c.cpp_compiler = "g++";
    c.cpp_standard = "-std=c++11";

    ifstream file(get_program_directory() + "/" + filename);
    if (!file.is_open()) {
        log_output(YELLOW, "No se pudieron cargar las configuraciones desde el archivo " + filename);
        log_output(WHITE, "");

        log_output(WHITE, "Se utilizaran las configuraciones por defecto: ");
        log_output(WHITE, "   time_limit = 1000");
        log_output(WHITE, "   mem_limit = 64000000");
        log_output(WHITE, "   io_format = auto");
        log_output(WHITE, "   enum_format = auto");
        log_output(WHITE, "   first_case = 1");
        log_output(WHITE, "   use_stdio = false");
        log_output(WHITE, "   cpp_compiler = g++");
        log_output(WHITE, "   cpp_standard = 11");
        log_output(WHITE, "");

        ofstream file(get_program_directory() + "/" + filename);
        file << "# Archivo de configuración para WTFaker\n\n";
        file << "# WTFaker también puede compilar y correr tu programa en caso\n";
        file << "# de que arrastres un archivo.cpp\n\n";
        file << "# El compilador que será utilizado para código .cpp\n";
        file << "# Esta opción puede especificar un path al copmilador\n";
        file << "# en el formato \"path_to/compiler.exe\" o \"path_to\\compiler.exe\"\n";
        file << "# En caso de que el compilador esté en el path\n";
        file << "# se puede especificar tal cuál\n";
        file << "# default = \"g++\"\n";
        file << "# compiladores compatibles: \"clang++\", \"g++\"\n";
        file << "cpp_compiler = \"g++\"\n\n";
        file << "# El estándar de C++ que será utilizado para la\n";
        file << "# compilación del código C++\n";
        file << "# default = 11\n";
        file << "cpp_standard = 11\n\n";
        file << "# Límite de tiempo\n";
        file << "# default = 1000\n";
        file << "# infinite -> 0\n";
        file << "time_limit = 1000\n\n";
        file << "# Límite de memoria\n";
        file << "# default = 64000000\n";
        file << "# infinite -> 0\n";
        file << "mem_limit = 64000000\n\n";
        file << "# formato de entrada / salida\n";
        file << "# 1 -> file.[num].in\n";
        file << "# 2 -> [num].in\n";
        file << "# 3 -> file.in.[num]\n";
        file << "# auto -> el programa revisará el formato de forma automatica\n";
        file << "# default = auto\n";
        file << "io_format = auto\n\n";
        file << "# formato de enumeración\n";
        file << "# 1 -> 1, 2, 3, ...\n";
        file << "# 2 -> 01, 02, 03, ...\n";
        file << "# auto -> el programa revisará el formato de forma automatica\n";
        file << "# default = auto\n";
        file << "enum_format = auto\n\n";
        file << "# primer caso de prueba\n";
        file << "# default = 1\n";
        file << "first_case = 1\n\n";
        file << "# define si el programa utilizará la salida estandar\n";
        file << "# true -> el programa espera entradas de consola\n";
        file << "# false -> el programa espera trabajar con archivos\n";
        file << "use_stdio = false\n";

        log_output(GREEN, "Se ha generado automaticamente el archivo " + filename);
        log_output(WHITE, "");
    }

    string input;
    u32 line = 1;
    while (getline(file, input)) {
        istringstream line_stream(input);
        string key;

        if (input.empty()) {
            goto skip;
        }

        if (!(line_stream >> key)) {
            goto skip;
        }

        if (key[0] == '#') {
            goto skip;
        }

        if (key == "time_limit") {
            string equals;
            if (!(line_stream >> equals) || equals != "=") {
                goto error;
            }
            if (!(line_stream >> c.time_limit)) {
                goto error;
            }
            goto skip;
        }
        else if (key == "mem_limit") {
            string equals;
            if (!(line_stream >> equals) || equals != "=") {
                goto error;
            }
            if (!(line_stream >> c.mem_limit)) {
                goto error;
            }
            goto skip;
        }
        else if (key == "io_format") {
            string equals;
            if (!(line_stream >> equals) || equals != "=") {
                goto error;
            }
            string f;
            if (!(line_stream >> f)) {
                goto error;
            }
            if (f == "3") {
                c.io_format = FILE_IN_NUM;
            }
            else if (f == "2") {
                c.io_format = NUM_IN;
            }
            else if (f == "1") {
                c.io_format = FILE_NUM_IN;
            }
            else {
                c.io_format = FILE_IN_AUTO;
            }
            goto skip;
        }
        else if (key == "enum_format") {
            string equals;
            if (!(line_stream >> equals) || equals != "=") {
                goto error;
            }
            string e;
            if (!(line_stream >> e)) {
                goto error;
            }
            if (e == "2") {
                c.ce_format = F_0X;
            }
            else if (e == "1") {
                c.ce_format = F_X;
            }
            else {
                c.ce_format = F_X_AUTO;
            }
            goto skip;
        }
        else if (key == "first_case") {
            string equals;
            if (!(line_stream >> equals) || equals != "=") {
                goto error;
            }
            if (!(line_stream >> c.first_case)) {
                goto error;
            }
            goto skip;
        }
        else if (key == "use_stdio") {
            string equals;
            if (!(line_stream >> equals) || equals != "=") {
                goto error;
            }
            string value;
            if (!(line_stream >> value)) {
                goto error;
            }
            c.use_stdio = (value == "true" || value == "TRUE" || value == "1");
            goto skip;
        }
        else if (key == "cpp_compiler") {
            string equals;
            if (!(line_stream >> equals) || equals != "=") {
                goto error;
            }
            string cpp;
            getline(line_stream, cpp);
            bool only_spaces = true;
            for (char& i : cpp) {
                if (i != ' ') {
                    only_spaces = false;
                    break;
                }
            }
            if (only_spaces) {
                goto error;
            }
            int fp = -1;
            for (int i = 0; i < cpp.size(); i++) {
                if (cpp[i] == '\"') {
                    if (fp == -1) {
                        fp = i;
                    }
                    else {
                        cpp = cpp.substr(fp, i - fp + 1);
                        break;
                    }
                }
            }

            only_spaces = true;
            for (int i = 1; i < cpp.size() - 1; i++) {
                if (i != ' ') {
                    only_spaces = false;
                    break;
                }
            }
            if (only_spaces) {
                goto error;
            }
            c.cpp_compiler = cpp;

            // log_output(WHITE, "\n\n" + cpp + "\n\n");
            goto skip;
        }
        else if (key == "cpp_standard") {
            string equals;
            if (!(line_stream >> equals) || equals != "=") {
                goto error;
            }
            string std;
            if (!(line_stream >> std)) {
                goto error;
            }
            string supported_std_version[] = {
                "98", "03", "11", "14", "17", "20", "23"
            };
            bool supported = false;
            for (string& i : supported_std_version) {
                if (i == std) {
                    supported = true;
                    break;
                }
            }

            if (!supported) {
                goto error;
            }
            c.cpp_standard = "-std=c++" + std;
            // log_output(WHITE, "\n\n" + ver + "\n\n");
            goto skip;
        }

        log_output(RED, "Clave desconocida en la linea: " + util::to_string(line));
        goto skip;

    error:
        log_output(RED, "Error de lectura en la linea: " + util::to_string(line));

    skip:
        line++;
    }
}

int main(int argc, char* argv[]) {
    wstring current_dir;
    wstring exe_path;
    string path_s;

    if (argc > 1) {
        exe_path = wstring(argv[1], argv[1] + strlen(argv[1]));
        path_s = string(exe_path.begin(), exe_path.end());

        size_t last_slash = exe_path.find_last_of(L"\\/");
        if (last_slash != wstring::npos) {
            current_dir = exe_path.substr(0, last_slash);
        }
        else {
            current_dir = L".";
        }

        log_output(WHITE, "Archivo arrastrado: " + path_s);
        log_output(WHITE, "Directorio: " + string(current_dir.begin(), current_dir.end()));
        log_output(WHITE, "");
    }

    config checker_conf;
    load_config("WTFaker.cfg", checker_conf);
    checker_conf.base_name = extract_base_name_from_wide(exe_path);

    string extension = "";
    if (path_s.size() > 3) {
        extension = path_s.substr(path_s.size() - 4);
    }
    if (extension == ".cpp" || extension == ".CPP") {
        log_output(CYAN, "Se ha detectado un archivo de codigo C++");
        log_output(CYAN, "Compilando el programa \"" + checker_conf.base_name + ".cpp\"...");

        string command = checker_conf.cpp_compiler + " " + checker_conf.cpp_standard + " ";
        command += checker_conf.base_name + ".cpp -static -o " + checker_conf.base_name + ".exe";
        int error_code = system(command.c_str());

        if (error_code != 0) {
            log_output(RED, "Se produjeron errores de compilacion, el programa no puede ser ejecutado");
            log_output(WHITE, "Pulsa cualquier tecla para cerrar...");
            getch();
            return -2;
        }

        log_output(CYAN, "El programa ha sido compilado correctamente\n");
        extension = ".exe";
    }
    if (extension == "" || (extension != ".exe" && extension != ".EXE")) {
        log_output(RED, "Se debe arrastrar un .exe o .cpp a este programa");
        log_output(WHITE, "Pulsa cualquier tecla para cerrar...");
        getch();
        return -1;
    }

    if (!set_working_directory(current_dir)) {
        return -1;
    }

    string program_path = checker_conf.base_name + ".exe";
    string input_file = checker_conf.base_name + ".in";
    string output_file = checker_conf.base_name + ".out";

    u32 case_num = checker_conf.first_case;
    u32 cases_executed = 0;
    u32 cases_ac = 0;
    u32 cases_wa = 0;
    u32 cases_tle = 0;
    u32 cases_mle = 0;
    u32 cases_rte = 0;

    if (checker_conf.time_limit > 0 && checker_conf.time_limit < 10) {
        log_output(YELLOW, "Limite de tiempo demasiado bajo, no se puede ejecutar el programa");
        log_output(WHITE, "Pulsa cualquier tecla para cerrar...");
        getch();
        return -1;
    }

    if (!file_exists(program_path)) {
        log_output(RED, "No se encuentra el programa especificado" + program_path);
        log_output(WHITE, "Pulsa cualquier tecla para cerrar...");
        getch();
        return -1;
    }

    vector <file_in_out_format> file_io_formats;
    if (checker_conf.io_format != FILE_IN_AUTO) {
        file_io_formats = { checker_conf.io_format };
    }
    else {
        file_io_formats = {
            FILE_NUM_IN,
            NUM_IN,
            FILE_IN_NUM
        };
    }
    vector <case_enum_format> enum_formats;
    if (checker_conf.ce_format != F_X_AUTO) {
        enum_formats = { checker_conf.ce_format };
    }
    else {
        enum_formats = {
            F_0X,
            F_X
        };
    }

    while (true) {
        string in_case, out_case;
        for (file_in_out_format i : file_io_formats) {
            for (case_enum_format j : enum_formats) {
                in_case = format_filename(checker_conf.base_name, case_num, i, j, true);
                out_case = format_filename(checker_conf.base_name, case_num, i, j, false);

                if (file_exists(in_case) && file_exists(out_case)) {
                    goto check_proceed;
                }
            }
        }

        // In case we can't proceed
        break;

        check_proceed:
        cases_executed++;
        if (case_num > checker_conf.first_case) {
            log_output(WHITE, string(50, '-'));
        }

        log_output(WHITE, "Caso #" + util::to_string(case_num) + ":");

        try {
            executer program(!checker_conf.use_stdio);
            program.set_limits(checker_conf.time_limit, checker_conf.mem_limit);
            program.set_working_directory(current_dir);

            if (!checker_conf.use_stdio) {
                if (!file_copy(in_case, input_file)) {
                    log_output(RED, "Error al copiar archivo de entrada");
                    cases_rte++;
                    case_num++;
                    continue;
                }

                if (!program.run(program_path, "", false)) {
                    log_output(CYAN, "Error al ejecutar el programa");
                    cases_rte++;
                    case_num++;
                    continue;
                }
            }
            else {
                text_loader input(in_case);
                input.load();

                if (!program.run(program_path, input.get_loaded_text(), true)) {
                    log_output(CYAN, "Error al ejecutar el programa");
                    cases_rte++;
                    case_num++;
                    continue;
                }
            }

            program.wait();

            log_output(program.was_tle() ? MAGENTA : GREEN, "Tiempo: " + util::to_string(program.getExecutionTime()) + " MS");
            log_output(program.was_mle() ? YELLOW : GREEN, "Memoria: " + util::to_string(program.getPeakMemory() / (1024.0f * 1024.0f)) + " MB");

            if (program.was_tle()) {
                log_output(MAGENTA, "TIME LIMIT EXCEEDED");
                cases_tle++;
            }
            else if (program.was_mle()) {
                log_output(YELLOW, "MEMORY LIMIT EXCEEDED");
                cases_mle++;
            }
            else if (program.was_rte()) {
                log_output(CYAN, "RUNTIME ERROR (codigo: " + util::to_string(program.get_exit_code()) + ")");
                cases_rte++;
            }
            else {
                bool passed = checker_conf.use_stdio ? test_passed(out_case, program.get_output(), false) : test_passed(out_case, output_file, true);
                if (passed) {
                    log_output(GREEN, "ACCEPTED");
                    cases_ac++;
                }
                else {
                    log_output(RED, "WRONG ANSWER");
                    cases_wa++;
                }
            }

        }
        catch (const exception& e) {
            log_output(RED, "Excepcion en caso #" + util::to_string(case_num) + ": " + string(e.what()));
            cases_rte++;
        }
        catch (...) {
            log_output(RED, "Error desconocido en caso #" + util::to_string(case_num));
            cases_rte++;
        }

        if (!checker_conf.use_stdio) {
            remove(input_file.c_str());
            remove(output_file.c_str());
        }
        case_num++;
    }

    if (cases_executed == 0) {
        log_output(RED, "No se encontraron casos de prueba");
        log_output(WHITE, "Pulsa cualquier tecla para cerrar...");
        getch();
        return -1;
    }

    log_output(WHITE, string(50, '='));
    log_output(WHITE, "Resultados:");
    log_output(WHITE,   "   Casos probados:      " + util::to_string(cases_executed));
    log_output(GREEN,   "   Aceptados:           " + util::to_string(cases_ac));
    log_output(RED,     "   Incorrectos:         " + util::to_string(cases_wa));
    log_output(MAGENTA, "   Excedido de tiempo:  " + util::to_string(cases_tle));
    log_output(YELLOW,  "   Excedido de memoria: " + util::to_string(cases_mle));
    log_output(CYAN,    "   Error de ejecucion:  " + util::to_string(cases_rte));

    f32 percentage = (cases_ac * 100.0f) / cases_executed;
    log_output(cases_ac == cases_executed ? GREEN : YELLOW, "Puntaje: " + util::to_string(percentage));

    log_output(WHITE, string(50, '='));
    log_output(WHITE, "Pulsa cualquier tecla para cerrar...");
    getch();
    return 0;
}

#include <chrono>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>
#include "benchmark/benchmarkresult.h"

using namespace std;

#ifdef _WIN32

#include <windows.h>

#else

#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <poll.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#endif


namespace benchmark {

// ============================================================
// ProcessResult
// ============================================================

struct ProcessResult {

    int exitCode = -1;

    string stdoutOutput;
    string stderrOutput;

    chrono::milliseconds runtime{0};

    bool timedOut = false;
    bool launchFailed = false;
};


// ============================================================
// ProcessManager
// ============================================================

class ProcessManager {

public:

    ProcessResult run(
        const string& executable,
        const vector<string>& arguments,
        chrono::milliseconds timeout
    );

};


// ============================================================
// WINDOWS IMPLEMENTATION
// ============================================================

#ifdef _WIN32

ProcessResult ProcessManager::run(
    const string& executable,
    const vector<string>& arguments,
    chrono::milliseconds timeout
) {

    ProcessResult result;


    SECURITY_ATTRIBUTES securityAttributes{};

    securityAttributes.nLength =
        sizeof(SECURITY_ATTRIBUTES);

    securityAttributes.bInheritHandle = TRUE;


    HANDLE stdoutRead = NULL;
    HANDLE stdoutWrite = NULL;

    HANDLE stderrRead = NULL;
    HANDLE stderrWrite = NULL;


    // --------------------------------------------------------
    // Create stdout pipe
    // --------------------------------------------------------

    if (!CreatePipe(
            &stdoutRead,
            &stdoutWrite,
            &securityAttributes,
            0
        )) {

        result.launchFailed = true;
        return result;
    }


    SetHandleInformation(
        stdoutRead,
        HANDLE_FLAG_INHERIT,
        0
    );


    // --------------------------------------------------------
    // Create stderr pipe
    // --------------------------------------------------------

    if (!CreatePipe(
            &stderrRead,
            &stderrWrite,
            &securityAttributes,
            0
        )) {

        CloseHandle(stdoutRead);
        CloseHandle(stdoutWrite);

        result.launchFailed = true;
        return result;
    }


    SetHandleInformation(
        stderrRead,
        HANDLE_FLAG_INHERIT,
        0
    );


    // --------------------------------------------------------
    // Build command line
    // --------------------------------------------------------

    string commandLine = "\"";
    commandLine += executable;
    commandLine += "\"";


    for (const string& argument : arguments) {

        commandLine += " \"";
        commandLine += argument;
        commandLine += "\"";
    }


    vector<char> commandBuffer(
        commandLine.begin(),
        commandLine.end()
    );

    commandBuffer.push_back('\0');


    // --------------------------------------------------------
    // Configure process startup
    // --------------------------------------------------------

    STARTUPINFOA startupInfo{};

    startupInfo.cb =
        sizeof(STARTUPINFOA);

    startupInfo.dwFlags =
        STARTF_USESTDHANDLES;

    startupInfo.hStdOutput =
        stdoutWrite;

    startupInfo.hStdError =
        stderrWrite;

    startupInfo.hStdInput =
        GetStdHandle(STD_INPUT_HANDLE);


    PROCESS_INFORMATION processInfo{};


    // --------------------------------------------------------
    // Start timer
    // --------------------------------------------------------

    auto start =
        chrono::steady_clock::now();


    // --------------------------------------------------------
    // Launch process
    // --------------------------------------------------------

    BOOL created = CreateProcessA(
        NULL,
        commandBuffer.data(),
        NULL,
        NULL,
        TRUE,
        0,
        NULL,
        NULL,
        &startupInfo,
        &processInfo
    );


    CloseHandle(stdoutWrite);
    CloseHandle(stderrWrite);


    if (!created) {

        CloseHandle(stdoutRead);
        CloseHandle(stderrRead);

        result.launchFailed = true;

        return result;
    }


    // --------------------------------------------------------
    // Monitor process and collect output
    // --------------------------------------------------------

    bool processFinished = false;


    while (!processFinished) {

        DWORD waitResult =
            WaitForSingleObject(
                processInfo.hProcess,
                20
            );


        char buffer[4096];

        DWORD bytesAvailable = 0;


        // ----------------------------------------------------
        // Read stdout
        // ----------------------------------------------------

        while (
            PeekNamedPipe(
                stdoutRead,
                NULL,
                0,
                NULL,
                &bytesAvailable,
                NULL
            ) &&
            bytesAvailable > 0
        ) {

            DWORD bytesRead = 0;


            if (
                ReadFile(
                    stdoutRead,
                    buffer,
                    sizeof(buffer),
                    &bytesRead,
                    NULL
                ) &&
                bytesRead > 0
            ) {

                result.stdoutOutput.append(
                    buffer,
                    bytesRead
                );
            }
            else {

                break;
            }
        }


        // ----------------------------------------------------
        // Read stderr
        // ----------------------------------------------------

        while (
            PeekNamedPipe(
                stderrRead,
                NULL,
                0,
                NULL,
                &bytesAvailable,
                NULL
            ) &&
            bytesAvailable > 0
        ) {

            DWORD bytesRead = 0;


            if (
                ReadFile(
                    stderrRead,
                    buffer,
                    sizeof(buffer),
                    &bytesRead,
                    NULL
                ) &&
                bytesRead > 0
            ) {

                result.stderrOutput.append(
                    buffer,
                    bytesRead
                );
            }
            else {

                break;
            }
        }


        // ----------------------------------------------------
        // Check timeout
        // ----------------------------------------------------

        auto elapsed =
            chrono::duration_cast<
                chrono::milliseconds
            >(
                chrono::steady_clock::now()
                - start
            );


        if (
            !processFinished &&
            elapsed >= timeout &&
            waitResult == WAIT_TIMEOUT
        ) {

            result.timedOut = true;


            TerminateProcess(
                processInfo.hProcess,
                1
            );


            WaitForSingleObject(
                processInfo.hProcess,
                INFINITE
            );


            processFinished = true;
        }


        if (waitResult == WAIT_OBJECT_0) {

            processFinished = true;
        }
    }


    // --------------------------------------------------------
    // Read remaining stdout/stderr
    // --------------------------------------------------------

    char buffer[4096];

    DWORD bytesAvailable = 0;


    while (
        PeekNamedPipe(
            stdoutRead,
            NULL,
            0,
            NULL,
            &bytesAvailable,
            NULL
        ) &&
        bytesAvailable > 0
    ) {

        DWORD bytesRead = 0;


        if (
            ReadFile(
                stdoutRead,
                buffer,
                sizeof(buffer),
                &bytesRead,
                NULL
            ) &&
            bytesRead > 0
        ) {

            result.stdoutOutput.append(
                buffer,
                bytesRead
            );
        }
        else {

            break;
        }
    }


    while (
        PeekNamedPipe(
            stderrRead,
            NULL,
            0,
            NULL,
            &bytesAvailable,
            NULL
        ) &&
        bytesAvailable > 0
    ) {

        DWORD bytesRead = 0;


        if (
            ReadFile(
                stderrRead,
                buffer,
                sizeof(buffer),
                &bytesRead,
                NULL
            ) &&
            bytesRead > 0
        ) {

            result.stderrOutput.append(
                buffer,
                bytesRead
            );
        }
        else {

            break;
        }
    }


    // --------------------------------------------------------
    // Get exit code
    // --------------------------------------------------------

    DWORD exitCode = 0;


    GetExitCodeProcess(
        processInfo.hProcess,
        &exitCode
    );


    if (!result.timedOut) {

        result.exitCode =
            static_cast<int>(exitCode);
    }


    // --------------------------------------------------------
    // Runtime
    // --------------------------------------------------------

    result.runtime =
        chrono::duration_cast<
            chrono::milliseconds
        >(
            chrono::steady_clock::now()
            - start
        );


    // --------------------------------------------------------
    // Cleanup
    // --------------------------------------------------------

    CloseHandle(stdoutRead);
    CloseHandle(stderrRead);

    CloseHandle(processInfo.hProcess);
    CloseHandle(processInfo.hThread);


    return result;
}


// ============================================================
// MACOS / LINUX IMPLEMENTATION
// ============================================================

#else

ProcessResult ProcessManager::run(
    const string& executable,
    const vector<string>& arguments,
    chrono::milliseconds timeout
) {

    ProcessResult result;


    int stdoutPipe[2];
    int stderrPipe[2];
    int execErrorPipe[2];


    // --------------------------------------------------------
    // Create stdout pipe
    // --------------------------------------------------------

    if (pipe(stdoutPipe) == -1) {

        result.launchFailed = true;
        return result;
    }


    // --------------------------------------------------------
    // Create stderr pipe
    // --------------------------------------------------------

    if (pipe(stderrPipe) == -1) {

        close(stdoutPipe[0]);
        close(stdoutPipe[1]);

        result.launchFailed = true;
        return result;
    }


    // --------------------------------------------------------
    // Create exec-error pipe
    // --------------------------------------------------------

    if (pipe(execErrorPipe) == -1) {

        close(stdoutPipe[0]);
        close(stdoutPipe[1]);

        close(stderrPipe[0]);
        close(stderrPipe[1]);

        result.launchFailed = true;
        return result;
    }


    // --------------------------------------------------------
    // Close execErrorPipe[1] automatically after successful
    // exec().
    // --------------------------------------------------------

    int flags =
        fcntl(
            execErrorPipe[1],
            F_GETFD
        );


    if (flags != -1) {

        fcntl(
            execErrorPipe[1],
            F_SETFD,
            flags | FD_CLOEXEC
        );
    }


    // --------------------------------------------------------
    // Fork
    // --------------------------------------------------------

    pid_t pid = fork();


    if (pid == -1) {

        close(stdoutPipe[0]);
        close(stdoutPipe[1]);

        close(stderrPipe[0]);
        close(stderrPipe[1]);

        close(execErrorPipe[0]);
        close(execErrorPipe[1]);

        result.launchFailed = true;

        return result;
    }


    // ========================================================
    // CHILD PROCESS
    // ========================================================

    if (pid == 0) {

        close(stdoutPipe[0]);
        close(stderrPipe[0]);
        close(execErrorPipe[0]);


        // ----------------------------------------------------
        // Redirect stdout
        // ----------------------------------------------------

        if (
            dup2(
                stdoutPipe[1],
                STDOUT_FILENO
            ) == -1
        ) {

            int errorCode = errno;

            write(
                execErrorPipe[1],
                &errorCode,
                sizeof(errorCode)
            );

            _exit(127);
        }


        // ----------------------------------------------------
        // Redirect stderr
        // ----------------------------------------------------

        if (
            dup2(
                stderrPipe[1],
                STDERR_FILENO
            ) == -1
        ) {

            int errorCode = errno;

            write(
                execErrorPipe[1],
                &errorCode,
                sizeof(errorCode)
            );

            _exit(127);
        }


        close(stdoutPipe[1]);
        close(stderrPipe[1]);


        // ----------------------------------------------------
        // Build argv
        // ----------------------------------------------------

        vector<char*> argv;

        argv.reserve(
            arguments.size() + 2
        );


        argv.push_back(
            const_cast<char*>(
                executable.c_str()
            )
        );


        for (
            const string& argument : arguments
        ) {

            argv.push_back(
                const_cast<char*>(
                    argument.c_str()
                )
            );
        }


        argv.push_back(nullptr);


        // ----------------------------------------------------
        // Execute solver
        // ----------------------------------------------------

        execvp(
            executable.c_str(),
            argv.data()
        );


        // ----------------------------------------------------
        // execvp() FAILED
        //
        // Tell the parent explicitly.
        // ----------------------------------------------------

        int errorCode = errno;


        write(
            execErrorPipe[1],
            &errorCode,
            sizeof(errorCode)
        );


        _exit(127);
    }


    // ========================================================
    // PARENT PROCESS
    // ========================================================

    close(stdoutPipe[1]);
    close(stderrPipe[1]);
    close(execErrorPipe[1]);


    // --------------------------------------------------------
    // Make stdout/stderr non-blocking
    // --------------------------------------------------------

    int stdoutFlags =
        fcntl(
            stdoutPipe[0],
            F_GETFL,
            0
        );


    int stderrFlags =
        fcntl(
            stderrPipe[0],
            F_GETFL,
            0
        );


    fcntl(
        stdoutPipe[0],
        F_SETFL,
        stdoutFlags | O_NONBLOCK
    );


    fcntl(
        stderrPipe[0],
        F_SETFL,
        stderrFlags | O_NONBLOCK
    );


    // --------------------------------------------------------
    // Start timer
    // --------------------------------------------------------

    auto start =
        chrono::steady_clock::now();


    bool processFinished = false;

    bool execFailureDetected = false;

    int waitStatus = 0;


    // --------------------------------------------------------
    // Monitor process
    // --------------------------------------------------------

    while (!processFinished) {

        pollfd descriptors[2];


        descriptors[0].fd =
            stdoutPipe[0];

        descriptors[0].events =
            POLLIN;


        descriptors[1].fd =
            stderrPipe[0];

        descriptors[1].events =
            POLLIN;


        poll(
            descriptors,
            2,
            20
        );


        // ----------------------------------------------------
        // Drain stdout
        // ----------------------------------------------------

        char buffer[4096];


        while (true) {

            ssize_t bytesRead =
                read(
                    stdoutPipe[0],
                    buffer,
                    sizeof(buffer)
                );


            if (bytesRead > 0) {

                result.stdoutOutput.append(
                    buffer,
                    bytesRead
                );
            }
            else {

                break;
            }
        }


        // ----------------------------------------------------
        // Drain stderr
        // ----------------------------------------------------

        while (true) {

            ssize_t bytesRead =
                read(
                    stderrPipe[0],
                    buffer,
                    sizeof(buffer)
                );


            if (bytesRead > 0) {

                result.stderrOutput.append(
                    buffer,
                    bytesRead
                );
            }
            else {

                break;
            }
        }


        // ----------------------------------------------------
        // Check whether execvp() failed
        // ----------------------------------------------------

        if (!execFailureDetected) {

            int errorCode = 0;


            ssize_t bytesRead =
                read(
                    execErrorPipe[0],
                    &errorCode,
                    sizeof(errorCode)
                );


            if (bytesRead > 0) {

                result.launchFailed = true;

                execFailureDetected = true;


                result.stderrOutput +=
                    "Failed to launch executable: ";


                result.stderrOutput +=
                    strerror(errorCode);


                result.stderrOutput += "\n";
            }
        }


        // ----------------------------------------------------
        // Check process status
        // ----------------------------------------------------

        pid_t waitResult =
            waitpid(
                pid,
                &waitStatus,
                WNOHANG
            );


        if (waitResult == pid) {

            processFinished = true;
        }


        // ----------------------------------------------------
        // Check timeout
        // ----------------------------------------------------

        auto elapsed =
            chrono::duration_cast<
                chrono::milliseconds
            >(
                chrono::steady_clock::now()
                - start
            );


        if (
            !processFinished &&
            elapsed >= timeout
        ) {

            result.timedOut = true;


            kill(
                pid,
                SIGKILL
            );


            waitpid(
                pid,
                &waitStatus,
                0
            );


            processFinished = true;
        }
    }


    // --------------------------------------------------------
    // Drain remaining stdout/stderr
    // --------------------------------------------------------

    char buffer[4096];


    while (true) {

        ssize_t bytesRead =
            read(
                stdoutPipe[0],
                buffer,
                sizeof(buffer)
            );


        if (bytesRead > 0) {

            result.stdoutOutput.append(
                buffer,
                bytesRead
            );
        }
        else {

            break;
        }
    }


    while (true) {

        ssize_t bytesRead =
            read(
                stderrPipe[0],
                buffer,
                sizeof(buffer)
            );


        if (bytesRead > 0) {

            result.stderrOutput.append(
                buffer,
                bytesRead
            );
        }
        else {

            break;
        }
    }


    // --------------------------------------------------------
    // Close pipes
    // --------------------------------------------------------

    close(stdoutPipe[0]);
    close(stderrPipe[0]);
    close(execErrorPipe[0]);


    // --------------------------------------------------------
    // Exit code
    // --------------------------------------------------------

    if (!result.timedOut &&
        !result.launchFailed) {

        if (WIFEXITED(waitStatus)) {

            result.exitCode =
                WEXITSTATUS(waitStatus);
        }

        else if (WIFSIGNALED(waitStatus)) {

            result.exitCode =
                128 + WTERMSIG(waitStatus);
        }
    }


    // --------------------------------------------------------
    // Runtime
    // --------------------------------------------------------

    result.runtime =
        chrono::duration_cast<
            chrono::milliseconds
        >(
            chrono::steady_clock::now()
            - start
        );


    return result;
}

#endif

// ============================================================
// runBenchmark
//
// Convenience wrapper: runs the solver via ProcessManager and
// immediately converts the raw ProcessResult into a structured
// BenchmarkResult. Does not do any solver-specific parsing.
// ============================================================

inline BenchmarkResult runBenchmark(
    ProcessManager& manager,
    const string& solverName,
    const string& instanceName,
    const string& executable,
    const vector<string>& arguments,
    chrono::milliseconds timeout
) {
    ProcessResult processResult =
        manager.run(executable, arguments, timeout);

    return buildBenchmarkResult(
        solverName,
        instanceName,
        processResult
    );
}

} // namespace benchmark
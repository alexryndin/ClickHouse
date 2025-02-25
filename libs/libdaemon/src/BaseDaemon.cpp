#include <daemon/BaseDaemon.h>
#include <Common/Config/ConfigProcessor.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/time.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <signal.h>
#include <cxxabi.h>
#include <execinfo.h>
#include <unistd.h>

#include <typeinfo>
#include <common/logger_useful.h>
#include <common/ErrorHandlers.h>
#include <common/Pipe.h>
#include <Common/StackTrace.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <iostream>
#include <fstream>
#include <sstream>
#include <memory>
#include <Poco/Observer.h>
#include <Poco/AutoPtr.h>
#include <common/getThreadNumber.h>
#include <Poco/PatternFormatter.h>
#include <Poco/TaskManager.h>
#include <Poco/File.h>
#include <Poco/Path.h>
#include <Poco/Message.h>
#include <Poco/Util/Application.h>
#include <Poco/Exception.h>
#include <Poco/ErrorHandler.h>
#include <Poco/Condition.h>
#include <Poco/SyslogChannel.h>
#include <Poco/DirectoryIterator.h>
#include <Common/Exception.h>
#include <IO/WriteBufferFromFile.h>
#include <IO/WriteBufferFromFileDescriptorDiscardOnFailure.h>
#include <IO/ReadBufferFromFileDescriptor.h>
#include <IO/ReadHelpers.h>
#include <IO/WriteHelpers.h>
#include <Common/getMultipleKeysFromConfig.h>
#include <Common/ClickHouseRevision.h>
#include <Common/config_version.h>
#include <common/argsToConfig.h>

#ifdef __APPLE__
// ucontext is not available without _XOPEN_SOURCE
#define _XOPEN_SOURCE 700
#endif
#include <ucontext.h>


Pipe signal_pipe;


/** Reset signal handler to the default and send signal to itself.
  * It's called from user signal handler to write core dump.
  */
static void call_default_signal_handler(int sig)
{
    signal(sig, SIG_DFL);
    raise(sig);
}


using ThreadNumber = decltype(getThreadNumber());
static const size_t buf_size = sizeof(int) + sizeof(siginfo_t) + sizeof(ucontext_t) + sizeof(StackTrace) + sizeof(ThreadNumber);

using signal_function = void(int, siginfo_t*, void*);

static void writeSignalIDtoSignalPipe(int sig)
{
    char buf[buf_size];
    DB::WriteBufferFromFileDescriptor out(signal_pipe.fds_rw[1], buf_size, buf);
    DB::writeBinary(sig, out);
    out.next();
}

/** Signal handler for HUP / USR1 */
static void closeLogsSignalHandler(int sig, siginfo_t * info, void * context)
{
    writeSignalIDtoSignalPipe(sig);
}

static void terminateRequestedSignalHandler(int sig, siginfo_t * info, void * context)
{
    writeSignalIDtoSignalPipe(sig);
}


/** Handler for "fault" signals. Send data about fault to separate thread to write into log.
  */
static void faultSignalHandler(int sig, siginfo_t * info, void * context)
{
    char buf[buf_size];
    DB::WriteBufferFromFileDescriptorDiscardOnFailure out(signal_pipe.fds_rw[1], buf_size, buf);

    const ucontext_t signal_context = *reinterpret_cast<ucontext_t *>(context);
    const StackTrace stack_trace(signal_context);

    DB::writeBinary(sig, out);
    DB::writePODBinary(*info, out);
    DB::writePODBinary(signal_context, out);
    DB::writePODBinary(stack_trace, out);
    DB::writeBinary(getThreadNumber(), out);

    out.next();

    if (sig != SIGPROF) /// This signal is used for debugging.
    {
        /// The time that is usually enough for separate thread to print info into log.
        ::sleep(10);
        call_default_signal_handler(sig);
    }
}


/** The thread that read info about signal or std::terminate from pipe.
  * On HUP / USR1, close log files (for new files to be opened later).
  * On information about std::terminate, write it to log.
  * On other signals, write info to log.
  */
class SignalListener : public Poco::Runnable
{
public:
    enum Signals : int
    {
        StdTerminate = -1,
        StopThread = -2
    };

    explicit SignalListener(BaseDaemon & daemon_)
        : log(&Logger::get("BaseDaemon"))
        , daemon(daemon_)
    {
    }

    void run()
    {
        char buf[buf_size];
        DB::ReadBufferFromFileDescriptor in(signal_pipe.fds_rw[0], buf_size, buf);

        while (!in.eof())
        {
            int sig = 0;
            DB::readBinary(sig, in);

            if (sig == Signals::StopThread)
            {
                LOG_INFO(log, "Stop SignalListener thread");
                break;
            }
            else if (sig == SIGHUP || sig == SIGUSR1)
            {
                LOG_DEBUG(log, "Received signal to close logs.");
                BaseDaemon::instance().closeLogs(BaseDaemon::instance().logger());
                LOG_INFO(log, "Opened new log file after received signal.");
            }
            else if (sig == Signals::StdTerminate)
            {
                ThreadNumber thread_num;
                std::string message;

                DB::readBinary(thread_num, in);
                DB::readBinary(message, in);

                onTerminate(message, thread_num);
            }
            else if (sig == SIGINT ||
                sig == SIGQUIT ||
                sig == SIGTERM)
            {
                daemon.handleSignal(sig);
            }
            else
            {
                siginfo_t info;
                ucontext_t context;
                StackTrace stack_trace(NoCapture{});
                ThreadNumber thread_num;

                DB::readPODBinary(info, in);
                DB::readPODBinary(context, in);
                DB::readPODBinary(stack_trace, in);
                DB::readBinary(thread_num, in);

                /// This allows to receive more signals if failure happens inside onFault function.
                /// Example: segfault while symbolizing stack trace.
                std::thread([=] { onFault(sig, info, context, stack_trace, thread_num); }).detach();
            }
        }
    }

private:
    Logger * log;
    BaseDaemon & daemon;

private:
    void onTerminate(const std::string & message, ThreadNumber thread_num) const
    {
        LOG_FATAL(log, "(version " << VERSION_STRING << VERSION_OFFICIAL << ") (from thread " << thread_num << ") " << message);
    }

    void onFault(int sig, const siginfo_t & info, const ucontext_t & context, const StackTrace & stack_trace, ThreadNumber thread_num) const
    {
        LOG_FATAL(log, "########################################");
        LOG_FATAL(log, "(version " << VERSION_STRING << VERSION_OFFICIAL << ") (from thread " << thread_num << ") "
            << "Received signal " << strsignal(sig) << " (" << sig << ")" << ".");

        LOG_FATAL(log, signalToErrorMessage(sig, info, context));

        if (stack_trace.getSize())
        {
            /// Write bare stack trace (addresses) just in case if we will fail to print symbolized stack trace.
            /// NOTE This still require memory allocations and mutex lock inside logger. BTW we can also print it to stderr using write syscalls.

            std::stringstream bare_stacktrace;
            bare_stacktrace << "Stack trace:";
            for (size_t i = stack_trace.getOffset(); i < stack_trace.getSize(); ++i)
                bare_stacktrace << ' ' << stack_trace.getFrames()[i];

            LOG_FATAL(log, bare_stacktrace.rdbuf());
        }

        /// Write symbolized stack trace line by line for better grep-ability.
        stack_trace.toStringEveryLine([&](const std::string & s) { LOG_FATAL(log, s); });
    }
};


/** To use with std::set_terminate.
  * Collects slightly more info than __gnu_cxx::__verbose_terminate_handler,
  *  and send it to pipe. Other thread will read this info from pipe and asynchronously write it to log.
  * Look at libstdc++-v3/libsupc++/vterminate.cc for example.
  */
static void terminate_handler()
{
    static thread_local bool terminating = false;
    if (terminating)
    {
        abort();
        return; /// Just for convenience.
    }

    terminating = true;

    std::string log_message;

    if (std::current_exception())
        log_message = "Terminate called for uncaught exception:\n" + DB::getCurrentExceptionMessage(true);
    else
        log_message = "Terminate called without an active exception";

    static const size_t buf_size = 1024;

    if (log_message.size() > buf_size - 16)
        log_message.resize(buf_size - 16);

    char buf[buf_size];
    DB::WriteBufferFromFileDescriptor out(signal_pipe.fds_rw[1], buf_size, buf);

    DB::writeBinary(static_cast<int>(SignalListener::StdTerminate), out);
    DB::writeBinary(getThreadNumber(), out);
    DB::writeBinary(log_message, out);
    out.next();

    abort();
}


static std::string createDirectory(const std::string & file)
{
    auto path = Poco::Path(file).makeParent();
    if (path.toString().empty())
        return "";
    Poco::File(path).createDirectories();
    return path.toString();
};


static bool tryCreateDirectories(Poco::Logger * logger, const std::string & path)
{
    try
    {
        Poco::File(path).createDirectories();
        return true;
    }
    catch (...)
    {
        LOG_WARNING(logger, __PRETTY_FUNCTION__ << ": when creating " << path << ", " << DB::getCurrentExceptionMessage(true));
    }
    return false;
}


void BaseDaemon::reloadConfiguration()
{
    /** If the program is not run in daemon mode and 'config-file' is not specified,
      *  then we use config from 'config.xml' file in current directory,
      *  but will log to console (or use parameters --log-file, --errorlog-file from command line)
      *  instead of using files specified in config.xml.
      * (It's convenient to log in console when you start server without any command line parameters.)
      */
    config_path = config().getString("config-file", "config.xml");
    DB::ConfigProcessor config_processor(config_path, false, true);
    config_processor.setConfigPath(Poco::Path(config_path).makeParent().toString());
    loaded_config = config_processor.loadConfig(/* allow_zk_includes = */ true);

    if (last_configuration != nullptr)
        config().removeConfiguration(last_configuration);
    last_configuration = loaded_config.configuration.duplicate();
    config().add(last_configuration, PRIO_DEFAULT, false);
}


BaseDaemon::BaseDaemon()
{
    checkRequiredInstructions();
}


BaseDaemon::~BaseDaemon()
{
    writeSignalIDtoSignalPipe(SignalListener::StopThread);
    signal_listener_thread.join();
    signal_pipe.close();
}


enum class InstructionFail
{
    NONE = 0,
    SSE3 = 1,
    SSSE3 = 2,
    SSE4_1 = 3,
    SSE4_2 = 4,
    AVX = 5,
    AVX2 = 6,
    AVX512 = 7
};

static std::string instructionFailToString(InstructionFail fail)
{
    switch(fail)
    {
        case InstructionFail::NONE:
            return "NONE";
        case InstructionFail::SSE3:
            return "SSE3";
        case InstructionFail::SSSE3:
            return "SSSE3";
        case InstructionFail::SSE4_1:
            return "SSE4.1";
        case InstructionFail::SSE4_2:
            return "SSE4.2";
        case InstructionFail::AVX:
            return "AVX";
        case InstructionFail::AVX2:
            return "AVX2";
        case InstructionFail::AVX512:
            return "AVX512";
    }
    __builtin_unreachable();
}


static sigjmp_buf jmpbuf;

static void sigIllCheckHandler(int sig, siginfo_t * info, void * context)
{
    siglongjmp(jmpbuf, 1);
}

/// Check if necessary sse extensions are available by trying to execute some sse instructions.
/// If instruction is unavailable, SIGILL will be sent by kernel.
static void checkRequiredInstructions(volatile InstructionFail & fail)
{
#if __SSE3__
    fail = InstructionFail::SSE3;
    __asm__ volatile ("addsubpd %%xmm0, %%xmm0" : : : "xmm0");
#endif

#if __SSSE3__
    fail = InstructionFail::SSSE3;
    __asm__ volatile ("pabsw %%xmm0, %%xmm0" : : : "xmm0");

#endif

#if __SSE4_1__
    fail = InstructionFail::SSE4_1;
    __asm__ volatile ("pmaxud %%xmm0, %%xmm0" : : : "xmm0");
#endif

#if __SSE4_2__
    fail = InstructionFail::SSE4_2;
    __asm__ volatile ("pcmpgtq %%xmm0, %%xmm0" : : : "xmm0");
#endif

#if __AVX__
    fail = InstructionFail::AVX;
    __asm__ volatile ("vaddpd %%ymm0, %%ymm0, %%ymm0" : : : "ymm0");
#endif

#if __AVX2__
    fail = InstructionFail::AVX2;
    __asm__ volatile ("vpabsw %%ymm0, %%ymm0" : : : "ymm0");
#endif

#if __AVX512__
    fail = InstructionFail::AVX512;
    __asm__ volatile ("vpabsw %%zmm0, %%zmm0" : : : "zmm0");
#endif

    fail = InstructionFail::NONE;
}


void BaseDaemon::checkRequiredInstructions()
{
    struct sigaction sa{};
    struct sigaction sa_old{};
    sa.sa_sigaction = sigIllCheckHandler;
    sa.sa_flags = SA_SIGINFO;
    auto signal = SIGILL;
    if (sigemptyset(&sa.sa_mask) != 0
        || sigaddset(&sa.sa_mask, signal) != 0
        || sigaction(signal, &sa, &sa_old) != 0)
    {
        std::cerr << "Can not set signal handler\n";
        exit(1);
    }

    volatile InstructionFail fail = InstructionFail::NONE;

    if (sigsetjmp(jmpbuf, 1))
    {
        std::cerr << "Instruction check fail. There is no " << instructionFailToString(fail) << " instruction set\n";
        exit(1);
    }

    ::checkRequiredInstructions(fail);

    if (sigaction(signal, &sa_old, nullptr))
    {
        std::cerr << "Can not set signal handler\n";
        exit(1);
    }
}


void BaseDaemon::terminate()
{
    getTaskManager().cancelAll();
    if (::raise(SIGTERM) != 0)
        throw Poco::SystemException("cannot terminate process");
}

void BaseDaemon::kill()
{
    pid.clear();
    if (::raise(SIGKILL) != 0)
        throw Poco::SystemException("cannot kill process");
}

void BaseDaemon::sleep(double seconds)
{
    wakeup_event.reset();
    wakeup_event.tryWait(seconds * 1000);
}

void BaseDaemon::wakeup()
{
    wakeup_event.set();
}

std::string BaseDaemon::getDefaultCorePath() const
{
    return "/opt/cores/";
}

void BaseDaemon::closeFDs()
{
#if defined(__FreeBSD__) || (defined(__APPLE__) && defined(__MACH__))
    Poco::File proc_path{"/dev/fd"};
#else
    Poco::File proc_path{"/proc/self/fd"};
#endif
    if (proc_path.isDirectory()) /// Hooray, proc exists
    {
        std::vector<std::string> fds;
        /// in /proc/self/fd directory filenames are numeric file descriptors
        proc_path.list(fds);
        for (const auto & fd_str : fds)
        {
            int fd = DB::parse<int>(fd_str);
            if (fd > 2 && fd != signal_pipe.fds_rw[0] && fd != signal_pipe.fds_rw[1])
                ::close(fd);
        }
    }
    else
    {
        int max_fd = -1;
#ifdef _SC_OPEN_MAX
        max_fd = sysconf(_SC_OPEN_MAX);
        if (max_fd == -1)
#endif
            max_fd = 256; /// bad fallback
        for (int fd = 3; fd < max_fd; ++fd)
            if (fd != signal_pipe.fds_rw[0] && fd != signal_pipe.fds_rw[1])
                ::close(fd);
    }
}

namespace
{
/// In debug version on Linux, increase oom score so that clickhouse is killed
/// first, instead of some service. Use a carefully chosen random score of 555:
/// the maximum is 1000, and chromium uses 300 for its tab processes. Ignore
/// whatever errors that occur, because it's just a debugging aid and we don't
/// care if it breaks.
#if defined(__linux__) && !defined(NDEBUG)
void debugIncreaseOOMScore()
{
    const std::string new_score = "555";
    try
    {
        DB::WriteBufferFromFile buf("/proc/self/oom_score_adj");
        buf.write(new_score.c_str(), new_score.size());
    }
    catch (const Poco::Exception & e)
    {
        LOG_WARNING(&Logger::root(), "Failed to adjust OOM score: '" +
                    e.displayText() + "'.");
        return;
    }
    LOG_INFO(&Logger::root(), "Set OOM score adjustment to " + new_score);
}
#else
void debugIncreaseOOMScore() {}
#endif
}

void BaseDaemon::initialize(Application & self)
{
    closeFDs();
    task_manager.reset(new Poco::TaskManager);
    ServerApplication::initialize(self);

    /// now highest priority (lowest value) is PRIO_APPLICATION = -100, we want higher!
    argsToConfig(argv(), config(), PRIO_APPLICATION - 100);

    bool is_daemon = config().getBool("application.runAsDaemon", false);

    if (is_daemon)
    {
        /** When creating pid file and looking for config, will search for paths relative to the working path of the program when started.
          */
        std::string path = Poco::Path(config().getString("application.path")).setFileName("").toString();
        if (0 != chdir(path.c_str()))
            throw Poco::Exception("Cannot change directory to " + path);
    }

    reloadConfiguration();

    /// This must be done before creation of any files (including logs).
    mode_t umask_num = 0027;
    if (config().has("umask"))
    {
        std::string umask_str = config().getString("umask");
        std::stringstream stream;
        stream << umask_str;
        stream >> std::oct >> umask_num;
    }
    umask(umask_num);

    DB::ConfigProcessor(config_path).savePreprocessedConfig(loaded_config, "");

    /// Write core dump on crash.
    {
        struct rlimit rlim;
        if (getrlimit(RLIMIT_CORE, &rlim))
            throw Poco::Exception("Cannot getrlimit");
        /// 1 GiB by default. If more - it writes to disk too long.
        rlim.rlim_cur = config().getUInt64("core_dump.size_limit", 1024 * 1024 * 1024);

        if (rlim.rlim_cur && setrlimit(RLIMIT_CORE, &rlim))
        {
            /// It doesn't work under address/thread sanitizer. http://lists.llvm.org/pipermail/llvm-bugs/2013-April/027880.html
            std::cerr << "Cannot set max size of core file to " + std::to_string(rlim.rlim_cur) << std::endl;
        }
    }

    /// This must be done before any usage of DateLUT. In particular, before any logging.
    if (config().has("timezone"))
    {
        if (0 != setenv("TZ", config().getString("timezone").data(), 1))
            throw Poco::Exception("Cannot setenv TZ variable");

        tzset();
    }

    std::string log_path = config().getString("logger.log", "");
    if (!log_path.empty())
        log_path = Poco::Path(log_path).setFileName("").toString();

    /** Redirect stdout, stderr to separate files in the log directory (or in the specified file).
      * Some libraries write to stderr in case of errors in debug mode,
      *  and this output makes sense even if the program is run in daemon mode.
      * We have to do it before buildLoggers, for errors on logger initialization will be written to these files.
      * If logger.stderr is specified then stderr will be forcibly redirected to that file.
      */
    if ((!log_path.empty() && is_daemon) || config().has("logger.stderr"))
    {
        std::string stderr_path = config().getString("logger.stderr", log_path + "/stderr.log");
        if (!freopen(stderr_path.c_str(), "a+", stderr))
            throw Poco::OpenFileException("Cannot attach stderr to " + stderr_path);
    }

    if ((!log_path.empty() && is_daemon) || config().has("logger.stdout"))
    {
        std::string stdout_path = config().getString("logger.stdout", log_path + "/stdout.log");
        if (!freopen(stdout_path.c_str(), "a+", stdout))
            throw Poco::OpenFileException("Cannot attach stdout to " + stdout_path);
    }

    /// Create pid file.
    if (config().has("pid"))
        pid.seed(config().getString("pid"));

    /// Change path for logging.
    if (!log_path.empty())
    {
        std::string path = createDirectory(log_path);
        if (is_daemon
            && chdir(path.c_str()) != 0)
            throw Poco::Exception("Cannot change directory to " + path);
    }
    else
    {
        if (is_daemon
            && chdir("/tmp") != 0)
            throw Poco::Exception("Cannot change directory to /tmp");
    }

    buildLoggers(config(), logger());

    if (is_daemon)
    {
        /** Change working directory to the directory to write core dumps.
          * We have to do it after buildLoggers, because there is the case when config files was in current directory.
          */

        std::string core_path = config().getString("core_path", "");
        if (core_path.empty())
            core_path = getDefaultCorePath();

        tryCreateDirectories(&logger(), core_path);

        Poco::File cores = core_path;
        if (!(cores.exists() && cores.isDirectory()))
        {
            core_path = !log_path.empty() ? log_path : "/opt/";
            tryCreateDirectories(&logger(), core_path);
        }

        if (0 != chdir(core_path.c_str()))
            throw Poco::Exception("Cannot change directory to " + core_path);
    }

    initializeTerminationAndSignalProcessing();
    logRevision();
    debugIncreaseOOMScore();

    for (const auto & key : DB::getMultipleKeysFromConfig(config(), "", "graphite"))
    {
        graphite_writers.emplace(key, std::make_unique<GraphiteWriter>(key));
    }
}


void BaseDaemon::initializeTerminationAndSignalProcessing()
{
    std::set_terminate(terminate_handler);

    /// We want to avoid SIGPIPE when working with sockets and pipes, and just handle return value/errno instead.
    {
        sigset_t sig_set;
        if (sigemptyset(&sig_set) || sigaddset(&sig_set, SIGPIPE) || pthread_sigmask(SIG_BLOCK, &sig_set, nullptr))
            throw Poco::Exception("Cannot block signal.");
    }

    /// Setup signal handlers.
    auto add_signal_handler =
        [](const std::vector<int> & signals, signal_function handler)
        {
            struct sigaction sa;
            memset(&sa, 0, sizeof(sa));
            sa.sa_sigaction = handler;
            sa.sa_flags = SA_SIGINFO;

            {
                if (sigemptyset(&sa.sa_mask))
                    throw Poco::Exception("Cannot set signal handler.");

                for (auto signal : signals)
                    if (sigaddset(&sa.sa_mask, signal))
                        throw Poco::Exception("Cannot set signal handler.");

                for (auto signal : signals)
                    if (sigaction(signal, &sa, nullptr))
                        throw Poco::Exception("Cannot set signal handler.");
            }
        };

    /// SIGPROF is added for debugging purposes. To output a stack trace of any running thread at anytime.

    add_signal_handler({SIGABRT, SIGSEGV, SIGILL, SIGBUS, SIGSYS, SIGFPE, SIGPIPE, SIGPROF}, faultSignalHandler);
    add_signal_handler({SIGHUP, SIGUSR1}, closeLogsSignalHandler);
    add_signal_handler({SIGINT, SIGQUIT, SIGTERM}, terminateRequestedSignalHandler);

    /// Set up Poco ErrorHandler for Poco Threads.
    static KillingErrorHandler killing_error_handler;
    Poco::ErrorHandler::set(&killing_error_handler);

    signal_listener.reset(new SignalListener(*this));
    signal_listener_thread.start(*signal_listener);

}

void BaseDaemon::logRevision() const
{
    Logger::root().information("Starting " + std::string{VERSION_FULL} + " with revision " + std::to_string(ClickHouseRevision::get()));
}

/// Makes server shutdown if at least one Poco::Task have failed.
void BaseDaemon::exitOnTaskError()
{
    Poco::Observer<BaseDaemon, Poco::TaskFailedNotification> obs(*this, &BaseDaemon::handleNotification);
    getTaskManager().addObserver(obs);
}

/// Used for exitOnTaskError()
void BaseDaemon::handleNotification(Poco::TaskFailedNotification *_tfn)
{
    task_failed = true;
    Poco::AutoPtr<Poco::TaskFailedNotification> fn(_tfn);
    Logger *lg = &(logger());
    LOG_ERROR(lg, "Task '" << fn->task()->name() << "' failed. Daemon is shutting down. Reason - " << fn->reason().displayText());
    ServerApplication::terminate();
}

void BaseDaemon::defineOptions(Poco::Util::OptionSet& _options)
{
    Poco::Util::ServerApplication::defineOptions (_options);

    _options.addOption(
        Poco::Util::Option("config-file", "C", "load configuration from a given file")
            .required(false)
            .repeatable(false)
            .argument("<file>")
            .binding("config-file"));

    _options.addOption(
        Poco::Util::Option("log-file", "L", "use given log file")
            .required(false)
            .repeatable(false)
            .argument("<file>")
            .binding("logger.log"));

    _options.addOption(
        Poco::Util::Option("errorlog-file", "E", "use given log file for errors only")
            .required(false)
            .repeatable(false)
            .argument("<file>")
            .binding("logger.errorlog"));

    _options.addOption(
        Poco::Util::Option("pid-file", "P", "use given pidfile")
            .required(false)
            .repeatable(false)
            .argument("<file>")
            .binding("pid"));
}

bool isPidRunning(pid_t pid)
{
    if (getpgid(pid) >= 0)
        return 1;
    return 0;
}

void BaseDaemon::PID::seed(const std::string & file_)
{
    file = Poco::Path(file_).absolute().toString();
    Poco::File poco_file(file);

    if (poco_file.exists())
    {
        pid_t pid_read = 0;
        {
            std::ifstream in(file);
            if (in.good())
            {
                in >> pid_read;
                if (pid_read && isPidRunning(pid_read))
                    throw Poco::Exception("Pid file exists and program running with pid = " + std::to_string(pid_read) + ", should not start daemon.");
            }
        }
        std::cerr << "Old pid file exists (with pid = " << pid_read << "), removing." << std::endl;
        poco_file.remove();
    }

    int fd = open(file.c_str(),
        O_CREAT | O_EXCL | O_WRONLY,
        S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH | S_IWOTH);

    if (-1 == fd)
    {
        file.clear();
        if (EEXIST == errno)
            throw Poco::Exception("Pid file exists, should not start daemon.");
        throw Poco::CreateFileException("Cannot create pid file.");
    }

    try
    {
        std::stringstream s;
        s << getpid();
        if (static_cast<ssize_t>(s.str().size()) != write(fd, s.str().c_str(), s.str().size()))
            throw Poco::Exception("Cannot write to pid file.");
    }
    catch (...)
    {
        close(fd);
        throw;
    }

    close(fd);
}

void BaseDaemon::PID::clear()
{
    if (!file.empty())
    {
        Poco::File(file).remove();
        file.clear();
    }
}

void BaseDaemon::handleSignal(int signal_id)
{
    if (signal_id == SIGINT ||
        signal_id == SIGQUIT ||
        signal_id == SIGTERM)
    {
        std::unique_lock<std::mutex> lock(signal_handler_mutex);
        {
            ++terminate_signals_counter;
            sigint_signals_counter += signal_id == SIGINT;
            signal_event.notify_all();
        }

        onInterruptSignals(signal_id);
    }
    else
        throw DB::Exception(std::string("Unsupported signal: ") + strsignal(signal_id), 0);
}

void BaseDaemon::onInterruptSignals(int signal_id)
{
    is_cancelled = true;
    LOG_INFO(&logger(), "Received termination signal (" << strsignal(signal_id) << ")");

    if (sigint_signals_counter >= 2)
    {
        LOG_INFO(&logger(), "Received second signal Interrupt. Immediately terminate.");
        kill();
    }
}


void BaseDaemon::waitForTerminationRequest()
{
    std::unique_lock<std::mutex> lock(signal_handler_mutex);
    signal_event.wait(lock, [this](){ return terminate_signals_counter > 0; });
}


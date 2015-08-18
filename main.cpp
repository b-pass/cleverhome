#include <unistd.h>
#include <sys/stat.h>
#include <signal.h>

#include <iostream>
#include <fstream>
#include <algorithm>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <ctime>

#include <boost/program_options.hpp>
#include <boost/algorithm/string.hpp>

#include <openzwave/Options.h>
#include <openzwave/Manager.h>
#include <openzwave/Driver.h>
#include <openzwave/Notification.h>
#include <openzwave/platform/Log.h>

using namespace OpenZWave;
namespace po = boost::program_options;

static const int TargetLuxValue = 50;

uint32_t g_homeId = 0;
std::mutex g_mutex;
std::condition_variable g_initCond;
std::atomic<bool> g_running(true), g_initComplete(false), g_switchOn(false);

ValueID nullValueID(0u, 0ull);
ValueID g_LuxLevel = nullValueID, g_DimmerLevel = nullValueID;

std::deque<std::pair<time_t, int>> g_luxHistory;
uint8_t g_CurDimmerLevel = 0;
float luxAverage = 0;
time_t dontDoAnythingUntil = 0;

std::vector<std::pair<time_t, std::function<void ()>>> m_action_queue;

#define CLEVER_LOG "/var/www/cleverhome/Log.txt"

void OnLuxLevel()
{
    std::ofstream out(CLEVER_LOG, std::ios::app);
    //std::cerr << "Lux level!" << std::endl;
    
    float thisLuxValue = 0;
    Manager::Get()->GetValueAsFloat(g_LuxLevel, &thisLuxValue);

    auto now = time(nullptr);
    struct tm nowtm;
    localtime_r(&now, &nowtm);
    auto oldest = now;
    while (!g_luxHistory.empty() && g_luxHistory.front().first + 300 < now)
        g_luxHistory.pop_front();
    g_luxHistory.emplace_back(now, thisLuxValue);
    oldest = g_luxHistory.front().first;

    auto oldLuxAverage = luxAverage;
    luxAverage = 0;
    for (auto&& tv : g_luxHistory)
        luxAverage += tv.second;
    luxAverage /= (float)g_luxHistory.size();

    static time_t lastLog = 0;
    if (lastLog + 60 < now || g_switchOn)
    {
        char when[256];
        strftime(when, sizeof(when), g_switchOn ? "%F %T" : "%F %R", localtime(&now));
        
        out << when << ": " << thisLuxValue << " lux  (" << luxAverage << " lux average)" << std::endl; 
        lastLog = now;
    }
    
    if (!g_initComplete)
        return;
    
    if ((now - oldest) < 60 || g_luxHistory.size() < 2)
        return;
    
    if (now < dontDoAnythingUntil)
        return;

    if (!g_switchOn &&
        nowtm.tm_hour < 18 && // don't turn on too late in summer
        oldLuxAverage > (TargetLuxValue + 5) &&
        luxAverage <= (TargetLuxValue + 5))
    {
        out << "It's getting dark in here so I'm turning the light on" << std::endl;
        g_CurDimmerLevel = 15;
        Manager::Get()->SetValue(g_DimmerLevel, g_CurDimmerLevel);
        
        Manager::Get()->SetNodeOn(g_DimmerLevel.GetHomeId(), g_DimmerLevel.GetNodeId());
        g_switchOn = true;
        
        Manager::Get()->SetValue(g_DimmerLevel, g_CurDimmerLevel);
        g_luxHistory.clear();
        return;
    }

    if (g_switchOn && luxAverage < TargetLuxValue && g_CurDimmerLevel < 100)
    {
        g_CurDimmerLevel += 1;
        if (luxAverage < (TargetLuxValue - 10))
            g_CurDimmerLevel += 3;
        out << "It's dark in here so I'm increasing the dimmer to " << (int)g_CurDimmerLevel << std::endl;
        Manager::Get()->SetValue(g_DimmerLevel, g_CurDimmerLevel);
        g_luxHistory.clear();
        return;
    }
    
    if (g_switchOn && luxAverage > (TargetLuxValue + 10))
    {
        g_luxHistory.clear();
        if (g_CurDimmerLevel > 10)
        {
            g_CurDimmerLevel -= 1;
            if (luxAverage > (TargetLuxValue + 20) && g_CurDimmerLevel > 5)
                g_CurDimmerLevel -= 3;
            out << "It's bright in here so I'm decreasing the dimmer to " << (int)g_CurDimmerLevel << std::endl;
            Manager::Get()->SetValue(g_DimmerLevel, g_CurDimmerLevel);
        }
        else
        {
            out << "It's plenty bright in here so turning the light off." << std::endl;
            Manager::Get()->SetValue(g_DimmerLevel, g_CurDimmerLevel=0);
            Manager::Get()->SetNodeOff(g_DimmerLevel.GetHomeId(), g_DimmerLevel.GetNodeId());
            g_switchOn = false;
        }
    }
}

void OnDimmerLevel()
{
    //std::cerr << "Dimmer level!" << std::endl;
    uint8_t level = 0;
    Manager::Get()->GetValueAsByte(g_DimmerLevel, &level);
    if (level != g_CurDimmerLevel && g_initComplete)
    {
        /*std::ofstream out(CLEVER_LOG, std::ios::app);
        out << "Dimmer level changed unexpectedly to " << (int)level << " (should've been "
            << (int)g_CurDimmerLevel << "), so not doing anything for 1.5 minutes." << std::endl;
        dontDoAnythingUntil = std::max(dontDoAnythingUntil, time(nullptr) + 90);*/
        
        m_action_queue.emplace_back(time(nullptr) + 4, [](){
            Manager::Get()->RefreshValue(g_DimmerLevel);
        });
    }
    g_CurDimmerLevel = level;
}

void OnButtonEvent(uint8_t eventLevel)
{
    //std::cerr << "Button event! " << (int)eventLevel << std::endl;
    
    g_switchOn = (eventLevel != 0);
    if (!g_initComplete)
        return;
    
    std::ofstream out(CLEVER_LOG, std::ios::app);
    out << "Light switched " << (eventLevel != 0 ? "on" : "off") 
        << ", so not doing anything for 2 minutes." << std::endl;
    dontDoAnythingUntil = std::max(dontDoAnythingUntil, time(nullptr) + 121);
    
    Manager::Get()->RefreshValue(g_DimmerLevel);
    g_luxHistory.clear();
}

void OnNotification(Notification const* notif, void* _context)
{
    // Must do this inside a critical section to avoid conflicts with the main thread
    std::unique_lock<std::mutex> lock(g_mutex);

    switch( notif->GetType() )
    {
        case Notification::Type_ValueAdded:
        {
            //std::cerr << "VALUE ADDED" << std::endl;
            auto&& vid = notif->GetValueID();
            auto&& name = Manager::Get()->GetValueLabel(vid);
            if (name == "Luminance")
            {
                g_LuxLevel = vid;
                std::cerr << "Found 'Lux' value" << std::endl;
            }
            else if (name == "Level" && vid.GetCommandClassId() == 38) // "COMMAND_CLASS_SWITCH_MULTILEVEL"
            {
                g_DimmerLevel = vid;
                std::cerr << "Found 'Level' level" << std::endl;
            }

            //if (name == "Group 1 Interval")
            //    Manager::Get()->SetValue(vid, 15);
            break;
        }

        case Notification::Type_ValueRemoved:
        {
            //std::cerr << "VALUE REMOVED" << std::endl;
            if (notif->GetValueID() == g_LuxLevel)
                g_LuxLevel = nullValueID;
            else if (notif->GetValueID() == g_DimmerLevel)
                g_DimmerLevel = nullValueID;
            break;
        }

        case Notification::Type_ValueRefreshed:
        case Notification::Type_ValueChanged:
        {
            //std::cerr << "VALUE CHANGED" << std::endl;
            if (notif->GetValueID() == g_LuxLevel)
                OnLuxLevel();
            else if (notif->GetValueID() == g_DimmerLevel)
                OnDimmerLevel();
            
            break;
        }

        case Notification::Type_NodeEvent:
        {
            // We have received an event from the node, caused by a
            // basic_set or hail message.
            /*std::cerr << "EVENT ON " << (int)notif->GetNodeId()
                << ", level = " << (int)notif->GetEvent()
                << std::endl;*/
            
            if (notif->GetNodeId() == g_DimmerLevel.GetNodeId())
                OnButtonEvent(notif->GetEvent());
            break;
        }

        case Notification::Type_Group:
        {
            //std::cerr << "GROUP" << std::endl;
            // One of the node's association groups has changed
            break;
        }

        case Notification::Type_NodeNew:
        case Notification::Type_NodeAdded:
        {
            //std::cerr << "NEW NODE " <<  (int)notif->GetNodeId() << std::endl;
            break;
        }

        case Notification::Type_NodeRemoved:
        {
            // Remove the node from our list
            //std::cerr << "BYE BYE " << (int)notif->GetNodeId() << std::endl;
            break;
        }

        case Notification::Type_PollingDisabled:
        {
            //std::cerr << "POLLING DISABLED ON " << (int)notif->GetNodeId() << std::endl;
            break;
        }

        case Notification::Type_PollingEnabled:
        {
            //std::cerr << "POLLING ENABLED ON " << (int)notif->GetNodeId() << std::endl;
            break;
        }

        case Notification::Type_DriverReady:
        {
            g_homeId = notif->GetHomeId();
            break;
        }

        case Notification::Type_DriverRemoved:
        case Notification::Type_DriverFailed:
        {
            g_running = false;
            g_initCond.notify_all();
            break;
        }

        case Notification::Type_AwakeNodesQueried:
        case Notification::Type_AllNodesQueried:
        case Notification::Type_AllNodesQueriedSomeDead:
        {
            g_initCond.notify_all();
            break;
        }

        case Notification::Type_DriverReset:
        case Notification::Type_Notification:
        case Notification::Type_NodeNaming:
        case Notification::Type_NodeProtocolInfo:
        {
            break;
        }
        case Notification::Type_EssentialNodeQueriesComplete:
        case Notification::Type_NodeQueriesComplete:
        {
            break;
        }
        
        case Notification::Type_CreateButton:
        case Notification::Type_DeleteButton:
        case Notification::Type_ButtonOn:
        case Notification::Type_ButtonOff:
        case Notification::Type_SceneEvent:
        {
            break;
        }
    }
}

void shutdown_signal(int sigNum)
{
    g_running = false;
}

//-----------------------------------------------------------------------------
// <main>
// Create the driver and then wait
//-----------------------------------------------------------------------------
int main( int argc, char* argv[] )
{
    std::string loglevel_str;
    
    po::options_description options("Options");
    options.add_options()
        ("help,h", "This help")
        ("reconfigure", "Re-send configuration information to devices")
        ("daemon,d", "Fork and run in the background")
        ("loglevel,l", po::value<std::string>(&loglevel_str)->default_value("info"), "OZW logging level (err, warn, info, debug)")
    ;

    po::variables_map argvars;
    po::store(po::parse_command_line(argc, argv, options), argvars);
    po::notify(argvars);

    if (argvars.count("help"))
    {
        std::cerr << options << std::endl;
        return 1;
    }

    boost::algorithm::to_lower(loglevel_str);
    int ozwLogLevel = LogLevel_Info;
    if (loglevel_str == "debug" || loglevel_str == "dbg")
        ozwLogLevel = LogLevel_Debug;
    else if (loglevel_str == "info" || loglevel_str == "inf")
        ozwLogLevel = LogLevel_Info;
    else if (loglevel_str == "warning" || loglevel_str == "warn")
        ozwLogLevel = LogLevel_Warning;
    else if (loglevel_str == "error" || loglevel_str == "err")
        ozwLogLevel = LogLevel_Error;
    else
        std::cerr << "Unknown log level: " << loglevel_str << std::endl;
    
    {
        std::ifstream ifs("/var/run/cleverhome.pid");
        pid_t old = -1;
        ifs >> old;
        if (old > 0)
        {
            errno = 0;
            if (kill(old, 0) != -1 || errno != ESRCH)
            {
                std::cerr << "Another instance of cleverhome is pid " << old << std::endl;
                _exit(1);
            }
        }
    }

    if (argvars.count("daemon"))
    {
        if (fork() != 0)
            _exit(0);
        if (fork() != 0)
            _exit(0);
    }

    {
        std::ofstream ofs("/var/run/cleverhome.pid");
        ofs << getpid() << std::endl;
    }

    bool do_reconfig = argvars.count("reconfigure") > 0;
    
    signal(SIGINT, &shutdown_signal);
    signal(SIGHUP, &shutdown_signal);
    
    std::cout << "Starting OpenZWave, Version " << Manager::getVersionAsString() << std::endl;
    
    char const *env = getenv("HOME");
    if (!env)
        env = ".";
    
    std::string ozwHome = env + std::string("/.openzwave");
    mkdir(ozwHome.c_str(), 0775);
    
    Options::Create("/usr/etc/openzwave", ozwHome, "");
    Options::Get()->AddOptionInt( "QueueLogLevel", ozwLogLevel );
    Options::Get()->AddOptionInt( "PollInterval", 300 );
    Options::Get()->Lock();

    Manager::Create();

    // Add a callback handler to the manager.  The second argument is a context that
    // is passed to the OnNotification method.  If the OnNotification is a method of
    // a class, the context would usually be a pointer to that class object, to
    // avoid the need for the notification handler to be a static.
    Manager::Get()->AddWatcher( OnNotification, NULL );

    // Add a Z-Wave Driver
    // Modify this line to set the correct serial port for your PC interface.

    std::string port = "/dev/ttyUSB0";
    if ( port == "usb" || port == "USB" || port == "HID" )
    {
        port = "HID Controller";
        Manager::Get()->AddDriver( "HID Controller", Driver::ControllerInterface_Hid );
    }
    else
    {
        Manager::Get()->AddDriver( port );
    }

    {
        std::unique_lock<std::mutex> lock(g_mutex);
        g_initCond.wait(lock);
        std::cout << "Init complete." << std::endl;
        g_initComplete = true;
    }

    Manager::Get()->SetChangeVerified(g_DimmerLevel, false);
    //Manager::Get()->RefreshValue(g_DimmerLevel);
    
    sleep(5);

    dontDoAnythingUntil = 0;
    {
        std::ofstream out(CLEVER_LOG, std::ios::app);
        out << "Startup complete" << std::endl;
    }

    if (do_reconfig)
    {
        std::cerr << "TODO: reconfiguration...\n"
            "Set sensor notify group, notify delay, notify type\n"
            "Add controller to notify group\n"
            "Anything in the switch?"
            ;
    }
    
    std::cout << "Now we're just waiting..." << std::endl;
    while (g_running)
    {
        sleep(1);

        {
            std::unique_lock<std::mutex> lock(g_mutex);
            auto now = time(nullptr);
            auto it = m_action_queue.begin();
            while (it != m_action_queue.end())
            {
                if (it->first <= now)
                {
                    it->second();
                    it = m_action_queue.erase(it);
                }
                else
                {
                    ++it;
                }
            }
        }
    }

    g_running = false;
    
    std::cout << "Shutting down..." << std::endl;
        
    /*Driver::DriverData data;
    Manager::Get()->GetDriverStatistics( g_homeId, &data );
    printf("SOF: %d ACK Waiting: %d Read Aborts: %d Bad Checksums: %d\n", data.m_SOFCnt, data.m_ACKWaiting, data.m_readAborts, data.m_badChecksum);
    printf("Reads: %d Writes: %d CAN: %d NAK: %d ACK: %d Out of Frame: %d\n", data.m_readCnt, data.m_writeCnt, data.m_CANCnt, data.m_NAKCnt, data.m_ACKCnt, data.m_OOFCnt);
    printf("Dropped: %d Retries: %d\n", data.m_dropped, data.m_retries);*/  

    // program exit (clean up)
    Manager::Get()->RemoveDriver( port );
    Manager::Get()->RemoveWatcher( OnNotification, NULL );
    Manager::Destroy();
    Options::Destroy();

    unlink("/var/run/cleverhome.pid");

    return 0;
}

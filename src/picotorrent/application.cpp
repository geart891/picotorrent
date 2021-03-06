#include "application.hpp"

#include <loguru.hpp>
#include <nlohmann/json.hpp>
#include <wx/cmdline.h>
#include <wx/ipc.h>
#include <wx/persist.h>
#include <wx/snglinst.h>
#include <wx/taskbarbutton.h>

#include "api/libpico_impl.hpp"
#include "crashpadinitializer.hpp"
#include "persistencemanager.hpp"
#include "core/configuration.hpp"
#include "core/database.hpp"
#include "core/environment.hpp"
#include "core/utils.hpp"
#include "ui/mainframe.hpp"
#include "ui/translator.hpp"

using json = nlohmann::json;
using pt::Application;

Application::Application()
    : wxApp(),
    m_singleInstance(std::make_unique<wxSingleInstanceChecker>())
{
    SetProcessDPIAware();
}

Application::~Application()
{
    for (auto plugin : m_plugins)
    {
        delete plugin;
    }
}

bool Application::OnCmdLineParsed(wxCmdLineParser& parser)
{
    for (size_t i = 0; i < parser.GetParamCount(); i++)
    {
        std::string arg = Utils::toStdString(parser.GetParam(i).ToStdWstring());

        if (arg.rfind("magnet:?xt", 0) == 0)
        {
            m_options.magnets.push_back(arg);
        }
        else
        {
            m_options.files.push_back(std::filesystem::absolute(arg).string());
        }
    }

    return true;
}

bool Application::OnInit()
{
    if (!wxApp::OnInit()) { return false; }

    if (m_singleInstance->IsAnotherRunning())
    {
        ActivateOtherInstance();
        return false;
    }

    auto env = pt::Core::Environment::Create();
    pt::CrashpadInitializer::Initialize(env);

    auto db = std::make_shared<pt::Core::Database>(env);

    if (!db->Migrate())
    {
        wxMessageBox(
            "Failed to run database migrations. Please check log file.",
            "PicoTorrent",
            wxICON_ERROR);
        return false;
    }

    auto cfg = std::make_shared<pt::Core::Configuration>(db);

    pt::UI::Translator& translator = pt::UI::Translator::GetInstance();
    translator.LoadEmbedded(GetModuleHandle(NULL));
    translator.SetLanguage(cfg->GetInt("language_id"));

    // Load plugins
    for (auto& p : fs::directory_iterator(env->GetApplicationPath()))
    {
        if (p.path().extension() != ".dll") { continue; }

        auto const& filename = p.path().filename().string();

        if (filename.size() < 6) { continue; }
        if (filename.substr(0, 6) != "Plugin") { continue; }

        LOG_F(INFO, "Loading plugin from %s", p.path().string().c_str());

        auto plugin = API::IPlugin::Load(p, env.get(), cfg.get());

        if (plugin != nullptr)
        {
            m_plugins.push_back(plugin);
        }
    }

    // Set up persistence manager
    m_persistence = std::make_unique<PersistenceManager>(db);
    wxPersistenceManager::Set(*m_persistence);

    auto mainFrame = new UI::MainFrame(env, db, cfg);

    std::for_each(
        m_plugins.begin(),
        m_plugins.end(),
        [mainFrame](auto plugin) { plugin->EmitEvent(libpico_event_mainwnd_created, mainFrame); });

    auto windowState = static_cast<pt::Core::Configuration::WindowState>(cfg->GetInt("start_position"));

    switch (windowState)
    {
    case pt::Core::Configuration::WindowState::Hidden:
        // Only valid if we have a notify icon
        if (cfg->GetBool("show_in_notification_area"))
        {
            mainFrame->MSWGetTaskBarButton()->Hide();
        }
        else
        {
            mainFrame->Show(true);
        }

        break;

    case pt::Core::Configuration::WindowState::Maximized:
        mainFrame->Show(true);
        mainFrame->Maximize();
        break;

    case pt::Core::Configuration::WindowState::Minimized:
        mainFrame->Iconize();
        mainFrame->Show(true);
        break;

    case pt::Core::Configuration::WindowState::Normal:
        mainFrame->Show(true);
        break;
    }

    mainFrame->HandleParams(
        m_options.files,
        m_options.magnets);

    return true;
}

void Application::OnInitCmdLine(wxCmdLineParser& parser)
{
    static const wxCmdLineEntryDesc cmdLineDesc[] =
    {
        { wxCMD_LINE_PARAM, NULL, NULL, "params", wxCMD_LINE_VAL_STRING, wxCMD_LINE_PARAM_OPTIONAL | wxCMD_LINE_PARAM_MULTIPLE },
        { wxCMD_LINE_NONE }
    };

    parser.SetDesc(cmdLineDesc);
    parser.SetSwitchChars("-");
}

void Application::ActivateOtherInstance()
{
    json j;
    j["files"] = m_options.files;
    j["magnet_links"] = m_options.magnets;

    wxClient client;
    auto conn = client.MakeConnection(
        "localhost",
        "PicoTorrent",
        "ApplicationOptions");

    if (conn)
    {
        conn->Execute(j.dump());
        conn->Disconnect();
    }
}

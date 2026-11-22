#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <functional>
#include <string>
#include <thread>
#include <utility>

#include <boost/asio/buffer.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/address.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core/buffers_to_string.hpp>
#include <boost/beast/core/error.hpp>
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/http/message.hpp>
#include <boost/beast/http/read.hpp>
#include <boost/beast/http/string_body.hpp>
#include <boost/beast/websocket/rfc6455.hpp>
#include <boost/beast/websocket/stream.hpp>
#include <nlohmann/json.hpp>

#include "slic3r/Utils/SnapmakerPrinterAgent.hpp"
#include "slic3r/Utils/bambu_networking.hpp"

using namespace Slic3r;
using nlohmann::json;

namespace {

namespace beast     = boost::beast;
namespace http      = beast::http;
namespace websocket = beast::websocket;
namespace net       = boost::asio;
using tcp           = net::ip::tcp;

PrintParams job(const std::string& mapping, const std::string& info = "")
{
    PrintParams params{};
    params.ams_mapping      = mapping;
    params.ams_mapping_info = info;
    return params;
}

// Serves one websocket call the way Moonraker does, sending a notification ahead of the answer.
class FakeU1
{
public:
    explicit FakeU1(json answer) : m_answer(std::move(answer))
    {
        m_thread = std::thread([this] { serve(); });
    }
    ~FakeU1() { finish(); }

    std::string base_url() const { return "http://127.0.0.1:" + std::to_string(m_port); }

    // Waits for the call to end; what it received may only be read afterwards.
    void finish()
    {
        if (!m_thread.joinable())
            return;
        // Releases the accept when the agent never called; otherwise this connection is never accepted.
        net::io_context   ioc;
        tcp::socket       wake{ioc};
        beast::error_code ignored;
        wake.connect({net::ip::make_address("127.0.0.1"), m_port}, ignored);
        wake.close(ignored);
        m_thread.join();
    }

    json        request;
    std::string target;
    std::string api_key;

private:
    void serve()
    {
        beast::error_code ec;
        tcp::socket       socket{m_ioc};
        m_acceptor.accept(socket, ec);
        if (ec)
            return;
        beast::flat_buffer               buffer;
        http::request<http::string_body> upgrade;
        http::read(socket, buffer, upgrade, ec);
        if (ec || !websocket::is_upgrade(upgrade))
            return;
        target  = std::string(upgrade.target().data(), upgrade.target().size());
        api_key = std::string(upgrade["X-Api-Key"].data(), upgrade["X-Api-Key"].size());

        websocket::stream<tcp::socket> ws{std::move(socket)};
        ws.accept(upgrade, ec);
        beast::flat_buffer message;
        if (!ec)
            ws.read(message, ec);
        if (ec)
            return;
        request = json::parse(beast::buffers_to_string(message.data()), nullptr, false);

        const std::string notification = json{{"jsonrpc", "2.0"}, {"method", "notify_proc_stat_update"}}.dump();
        ws.write(net::buffer(notification), ec);
        json response       = m_answer;
        response["jsonrpc"] = "2.0";
        response["id"]      = request.value("id", 0);
        const std::string answer = response.dump();
        if (!ec)
            ws.write(net::buffer(answer), ec);
        beast::flat_buffer rest;
        if (!ec)
            ws.read(rest, ec); // until the agent hangs up
    }

    const json           m_answer;
    net::io_context      m_ioc;
    tcp::acceptor        m_acceptor{m_ioc, {net::ip::make_address("127.0.0.1"), 0}};
    const unsigned short m_port{m_acceptor.local_endpoint().port()};
    std::thread          m_thread;
};

class StartAgent : public SnapmakerPrinterAgent
{
public:
    explicit StartAgent(const FakeU1& printer) : SnapmakerPrinterAgent("")
    {
        device_info.base_url = printer.base_url();
        device_info.api_key  = "test-key";
    }
    using SnapmakerPrinterAgent::print_uploaded_file;
};

} // namespace

TEST_CASE("U1 print options pair each used filament with its chosen toolhead", "[SnapmakerPrinterAgent]")
{
    json        options;
    std::string error;
    REQUIRE(SnapmakerPrinterAgent::build_print_options(job("[2,-1,0,3,2]"), options, error));

    CHECK(options.at("map_table") == json::parse("[[0,2],[2,0],[3,3],[4,2]]"));
    CHECK(error.empty());
}

TEST_CASE("U1 print options carry the dialog's switches as integers", "[SnapmakerPrinterAgent]")
{
    const bool on = GENERATE(false, true);
    CAPTURE(on);
    PrintParams params           = job("[0]");
    params.task_bed_leveling     = on;
    params.task_flow_cali        = on;
    params.task_record_timelapse = on;

    json        options;
    std::string error;
    REQUIRE(SnapmakerPrinterAgent::build_print_options(params, options, error));

    for (const char* key : {"bed_level", "flow_calibrate", "time_lapse_camera"}) {
        CAPTURE(key);
        // Klipper rejects "True" where it reads an integer
        CHECK(options.at(key).is_number_integer());
        CHECK(options.at(key).get<int>() == (on ? 1 : 0));
    }
}

TEST_CASE("A U1 job without a mapping leaves the table for the printer to reset", "[SnapmakerPrinterAgent]")
{
    json        options;
    std::string error;
    REQUIRE(SnapmakerPrinterAgent::build_print_options(job(""), options, error));

    CHECK_FALSE(options.contains("map_table"));
}

TEST_CASE("U1 print options map the last of the 32 logical tools", "[SnapmakerPrinterAgent]")
{
    json mapping = json::array();
    for (int i = 0; i < 31; ++i)
        mapping.push_back(-1);
    mapping.push_back(1);

    json        options;
    std::string error;
    REQUIRE(SnapmakerPrinterAgent::build_print_options(job(mapping.dump()), options, error));

    CHECK(options.at("map_table") == json::parse("[[31,1]]"));
}

TEST_CASE("U1 print options refuse a mapping the printer cannot honour", "[SnapmakerPrinterAgent]")
{
    std::string beyond_last_tool = "[";
    for (int i = 0; i < 32; ++i)
        beyond_last_tool += "-1,";
    beyond_last_tool += "0]";

    const std::string mapping = GENERATE_COPY(std::string("not json"), std::string("[4]"), std::string("[-2]"),
                                              std::string("[\"0\"]"), std::string("[-1,-1]"), beyond_last_tool);
    CAPTURE(mapping);

    json        options;
    std::string error;
    CHECK_FALSE(SnapmakerPrinterAgent::build_print_options(job(mapping), options, error));
    CHECK_FALSE(error.empty());
}

TEST_CASE("U1 print options refuse a used filament that has no toolhead", "[SnapmakerPrinterAgent]")
{
    const std::string info = R"([{"ams":0,"sourceColor":"FF0000FF"},{"ams":255,"sourceColor":"00FF00FF"}])";

    json        options;
    std::string error;
    CHECK_FALSE(SnapmakerPrinterAgent::build_print_options(job("[0,-1]", info), options, error));
    CHECK_FALSE(error.empty());
}

TEST_CASE("The Snapmaker agent starts a print over the websocket with the chosen toolhead map", "[SnapmakerPrinterAgent]")
{
    FakeU1      printer(json::parse(R"({"result":{"state":"success","message":"Print started"}})"));
    StartAgent  agent(printer);
    std::string error;

    const bool started = agent.print_uploaded_file(job("[3,-1,1]"), "my part.gcode", error);
    printer.finish();

    CHECK(started);
    CHECK(error.empty());
    CHECK(printer.target == "/websocket");
    CHECK(printer.api_key == "test-key");
    REQUIRE(printer.request.is_object());
    CHECK(printer.request.at("method") == "server.files.start_local_print");
    CHECK(printer.request.at("params").at("path") == "my part.gcode");
    CHECK(printer.request.at("params").at("options").at("map_table") == json::parse("[[0,3],[2,1]]"));
}

TEST_CASE("The Snapmaker agent reports a refused start with the printer's reason", "[SnapmakerPrinterAgent]")
{
    const std::string answer = GENERATE(std::string(R"({"result":{"state":"error","message":"Printer is busy"}})"),
                                        std::string(R"({"error":{"code":400,"message":"Printer is busy"}})"));
    CAPTURE(answer);
    FakeU1      printer(json::parse(answer));
    StartAgent  agent(printer);
    std::string error;

    CHECK_FALSE(agent.print_uploaded_file(job("[0]"), "part.gcode", error));
    CHECK(error == "Printer is busy");
}

TEST_CASE("The Snapmaker agent asks for a firmware update when the start method is missing", "[SnapmakerPrinterAgent]")
{
    FakeU1      printer(json::parse(R"({"error":{"code":-32601,"message":"Method not found"}})"));
    StartAgent  agent(printer);
    std::string error;

    CHECK_FALSE(agent.print_uploaded_file(job("[0]"), "part.gcode", error));
    CHECK(error.find("1.3.0") != std::string::npos);
}

TEST_CASE("The Snapmaker agent does not call the printer for a mapping it cannot honour", "[SnapmakerPrinterAgent]")
{
    FakeU1      printer(json::parse(R"({"result":{"state":"success"}})"));
    StartAgent  agent(printer);
    std::string error;

    CHECK_FALSE(agent.print_uploaded_file(job("[7]"), "part.gcode", error));
    printer.finish();
    CHECK(printer.request.is_null());
}

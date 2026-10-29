#include "exchangecore/benchmark.hpp"
#include "exchangecore/csv.hpp"
#include "exchangecore/matching_engine.hpp"
#include "exchangecore/price.hpp"

#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <string_view>
#include <vector>

namespace ec = exchangecore;
namespace {
constexpr std::string_view usage = R"(ExchangeCore — deterministic C++20 price-time matching engine
Usage:
  exchangecore demo
  exchangecore replay FILE.csv [--quiet]
  exchangecore validate FILE.csv
  exchangecore benchmark [--events N] [--trials N] [--warmup N]
      [--samples N] [--seed N] [--reserve N] [--workload NAME] [--csv]
  exchangecore --help

CSV: type,id,side,quantity,price (price is positive integer ticks; 1 tick=0.25)
  LIMIT,1,SELL,10,24001
  MARKET,2,BUY,4,
  CANCEL,1,,,
Bench workloads: all, resting, crossing, cancellation, deep, touch, mixed.
Exit codes: 0 success; 1 file/runtime error; 2 usage, syntax or event rejection.
)";

std::string_view reason(ec::RejectReason r) {
    using enum ec::RejectReason;
    switch (r) {
        case None: return "none";
        case InvalidId: return "invalid ID";
        case InvalidSide: return "invalid side";
        case InvalidType: return "invalid order type";
        case InvalidQuantity: return "invalid quantity";
        case InvalidPrice: return "invalid price";
        case DuplicateId: return "duplicate ID";
        case UnknownOrder: return "unknown active order";
        case CapacityExceeded: return "accepted ID capacity exceeded";
        case QuantityOverflow: return "resting quantity would overflow";
        case SequenceExhausted: return "arrival sequence exhausted";
        case InvalidEvent: return "invalid event";
    }
    return "unknown rejection";
}

void print_result(const ec::Event& e, const ec::Result& r) {
    std::cout << (e.type == ec::EventType::Cancel ? "CANCEL" :
        e.order.type == ec::OrderType::Limit ? "LIMIT" : "MARKET")
        << " id=" << r.order_id << ' ' << (r.accepted ? "ACCEPTED" : "REJECTED");
    if (!r.accepted) std::cout << " reason=" << reason(r.reason);
    std::cout << " filled=" << r.filled << " remaining=" << r.remaining
        << " canceled=" << r.canceled << '\n';
    for (const auto& t : r.trades)
        std::cout << "  TRADE maker=" << t.maker_id << " taker=" << t.taker_id
            << " quantity=" << t.quantity << " price=" << ec::format_price(t.price)
            << " ticks=" << t.price << '\n';
}

void print_book(const ec::MatchingEngine& engine) {
    const auto price = [](std::optional<ec::Price> p) { return p ? ec::format_price(*p) : "EMPTY"; };
    std::cout << "BOOK best_bid=" << price(engine.best_bid())
        << " best_ask=" << price(engine.best_ask())
        << " active_orders=" << engine.active_order_count()
        << " resting_quantity=" << engine.resting_quantity() << '\n';
    for (const auto side : {ec::Side::Buy, ec::Side::Sell}) {
        const auto depth = engine.depth(side, 10);
        for (const auto& level : depth)
            std::cout << "  " << (side == ec::Side::Buy ? "BID" : "ASK")
                << " price=" << ec::format_price(level.price) << " ticks=" << level.price
                << " quantity=" << level.quantity << " orders=" << level.order_count << '\n';
        if (depth.size() == 10) std::cout << "  (depth display capped at 10 levels per side)\n";
    }
}

int demo() {
    const std::vector events{
        ec::Event::limit(1, ec::Side::Sell, 10, 24001),
        ec::Event::limit(2, ec::Side::Sell, 5, 24001),
        ec::Event::limit(3, ec::Side::Sell, 8, 24002),
        ec::Event::limit(4, ec::Side::Buy, 4, 24001),
        ec::Event::limit(5, ec::Side::Buy, 9, 24001),
        ec::Event::limit(6, ec::Side::Buy, 7, 24002),
        ec::Event::limit(7, ec::Side::Buy, 6, 23999),
        ec::Event::cancel(7),
        ec::Event::limit(8, ec::Side::Buy, 3, 24000)};
    ec::MatchingEngine engine;
    std::cout << "Synthetic ES demo (1 tick = 0.25 index points)\n";
    std::size_t trades = 0;
    for (const auto& event : events) {
        const auto result = engine.process(event);
        trades += result.trades.size();
        print_result(event, result);
        print_book(engine);
    }
    engine.check_invariants();
    std::cout << "SUMMARY events=" << events.size() << " rejected=0 trades=" << trades << '\n';
    return 0;
}

int replay(const char* path, bool validate, bool quiet) {
    std::error_code path_error;
    if (std::filesystem::is_directory(path, path_error)) {
        std::cerr << path << ": input path is a directory\n";
        return 1;
    }
    std::ifstream input(path);
    if (!input) { std::cerr << path << ": cannot open input file\n"; return 1; }
    const auto parsed = ec::parse_csv(input);
    for (const auto& e : parsed.errors)
        std::cerr << path << ':' << e.line << ": " << e.message << '\n';
    if (!parsed.errors.empty())
        return input.bad() || (input.fail() && !input.eof()) ? 1 : 2;
    ec::MatchingEngine engine;
    std::size_t rejected = 0;
    std::size_t trades = 0;
    for (const auto& row : parsed.events) {
        const auto r = engine.process(row.event);
        trades += r.trades.size();
        if (!r.accepted) {
            ++rejected;
            std::cerr << path << ':' << row.line << ": " << reason(r.reason) << '\n';
        }
        if (!validate && !quiet) print_result(row.event, r);
    }
    engine.check_invariants();
    if (!validate) print_book(engine);
    std::cout << (validate ? "VALIDATION" : "SUMMARY") << " events=" << parsed.events.size()
        << " accepted=" << parsed.events.size() - rejected << " rejected=" << rejected
        << " trades=" << trades << '\n';
    return rejected == 0 ? 0 : 2;
}

} // namespace

int main(int argc, char** argv) {
    try {
        if (argc == 1 || (argc == 2 && (std::string_view(argv[1]) == "--help" || std::string_view(argv[1]) == "help"))) {
            std::cout << usage; return 0;
        }
        const std::string_view command = argv[1];
        if (command == "demo" && argc == 2) return demo();
        if (command == "replay" && (argc == 3 || (argc == 4 && std::string_view(argv[3]) == "--quiet")))
            return replay(argv[2], false, argc == 4);
        if (command == "validate" && argc == 3) return replay(argv[2], true, true);
        if (command == "benchmark") return ec::run_benchmark_cli(argc - 2, argv + 2, std::cout, std::cerr);
        std::cerr << "invalid arguments; use exchangecore --help\n"; return 2;
    } catch (const std::exception& e) {
        std::cerr << "exchangecore: " << e.what() << '\n'; return 1;
    }
}

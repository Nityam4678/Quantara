#include "lob/order_book.hpp"

#include <charconv>
#include <cctype>
#include <cstdint>
#include <deque>
#include <iostream>
#include <limits>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

using Fields = std::map<std::string, std::string>;

std::string json_escape(std::string_view value) {
    std::string result;
    result.reserve(value.size() + 2);
    for (const char character : value) {
        switch (character) {
        case '"': result += "\\\""; break;
        case '\\': result += "\\\\"; break;
        case '\n': result += "\\n"; break;
        case '\r': result += "\\r"; break;
        case '\t': result += "\\t"; break;
        default:
            if (static_cast<unsigned char>(character) < 0x20) {
                throw std::runtime_error("control character in JSON string");
            }
            result += character;
        }
    }
    return result;
}

void skip_space(std::string_view input, std::size_t& position) {
    while (position < input.size() &&
           std::isspace(static_cast<unsigned char>(input[position])) != 0) {
        ++position;
    }
}

std::string parse_string(std::string_view input, std::size_t& position) {
    if (position >= input.size() || input[position] != '"') {
        throw std::runtime_error("expected JSON string");
    }
    ++position;
    std::string result;
    while (position < input.size()) {
        const char character = input[position++];
        if (character == '"') return result;
        if (character == '\\') {
            if (position >= input.size()) throw std::runtime_error("incomplete escape");
            const char escaped = input[position++];
            switch (escaped) {
            case '"': result += '"'; break;
            case '\\': result += '\\'; break;
            case '/': result += '/'; break;
            case 'b': result += '\b'; break;
            case 'f': result += '\f'; break;
            case 'n': result += '\n'; break;
            case 'r': result += '\r'; break;
            case 't': result += '\t'; break;
            default: throw std::runtime_error("unsupported JSON escape");
            }
        } else {
            if (static_cast<unsigned char>(character) < 0x20) {
                throw std::runtime_error("control character in JSON string");
            }
            result += character;
        }
    }
    throw std::runtime_error("unterminated JSON string");
}

std::string parse_scalar(std::string_view input, std::size_t& position) {
    if (position < input.size() && input[position] == '"') {
        return parse_string(input, position);
    }
    const std::size_t start = position;
    while (position < input.size() &&
           input[position] != ',' && input[position] != '}' &&
           std::isspace(static_cast<unsigned char>(input[position])) == 0) {
        ++position;
    }
    if (start == position) throw std::runtime_error("expected JSON scalar");
    return std::string(input.substr(start, position - start));
}

Fields parse_object(std::string_view input) {
    std::size_t position = 0;
    Fields fields;
    skip_space(input, position);
    if (position >= input.size() || input[position++] != '{') {
        throw std::runtime_error("request must be a JSON object");
    }
    skip_space(input, position);
    if (position < input.size() && input[position] == '}') {
        ++position;
    } else {
        for (;;) {
            skip_space(input, position);
            const std::string key = parse_string(input, position);
            skip_space(input, position);
            if (position >= input.size() || input[position++] != ':') {
                throw std::runtime_error("expected colon");
            }
            skip_space(input, position);
            if (!fields.emplace(key, parse_scalar(input, position)).second) {
                throw std::runtime_error("duplicate JSON field");
            }
            skip_space(input, position);
            if (position >= input.size()) throw std::runtime_error("unterminated object");
            if (input[position] == '}') {
                ++position;
                break;
            }
            if (input[position++] != ',') throw std::runtime_error("expected comma");
        }
    }
    skip_space(input, position);
    if (position != input.size()) throw std::runtime_error("trailing JSON data");
    return fields;
}

const std::string& required(const Fields& fields, std::string_view key) {
    const auto it = fields.find(std::string(key));
    if (it == fields.end() || it->second.empty()) {
        throw std::runtime_error("missing field: " + std::string(key));
    }
    return it->second;
}

std::uint64_t unsigned_value(const Fields& fields, std::string_view key) {
    const std::string& value = required(fields, key);
    std::uint64_t result = 0;
    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), result);
    if (error != std::errc{} || end != value.data() + value.size()) {
        throw std::runtime_error("invalid unsigned field: " + std::string(key));
    }
    return result;
}

std::size_t bounded_limit(const Fields& fields, std::string_view key, std::size_t maximum) {
    const std::uint64_t value = unsigned_value(fields, key);
    if (value > maximum) throw std::runtime_error("field exceeds configured maximum: " + std::string(key));
    return static_cast<std::size_t>(value);
}

std::string quoted(std::string_view value) {
    return "\"" + json_escape(value) + "\"";
}

std::string side_name(lob::Side side) {
    return side == lob::Side::BUY ? "BUY" : "SELL";
}

std::string status_name(lob::OrderStatus status) {
    switch (status) {
    case lob::OrderStatus::Ok: return "OK";
    case lob::OrderStatus::RejectedDuplicateId: return "REJECTED_DUPLICATE_ID";
    case lob::OrderStatus::RejectedZeroQuantity: return "REJECTED_ZERO_QUANTITY";
    case lob::OrderStatus::RejectedZeroPrice: return "REJECTED_ZERO_PRICE";
    case lob::OrderStatus::RejectedPriceOutOfRange: return "REJECTED_PRICE_OUT_OF_RANGE";
    case lob::OrderStatus::RejectedPoolExhausted: return "REJECTED_POOL_EXHAUSTED";
    case lob::OrderStatus::CancelUnknownId: return "CANCEL_UNKNOWN_ID";
    }
    return "UNKNOWN";
}

std::string report_json(const lob::ExecutionReport& report) {
    std::ostringstream output;
    output << "{\"orderId\":" << report.order_id
           << ",\"status\":" << quoted(status_name(report.status))
           << ",\"filled\":" << report.filled
           << ",\"resting\":" << report.resting << "}";
    return output.str();
}

std::string trade_json(const lob::Trade& trade) {
    std::ostringstream output;
    output << "{\"makerOrderId\":" << trade.maker_order_id
           << ",\"takerOrderId\":" << trade.taker_order_id
           << ",\"price\":" << trade.price
           << ",\"quantity\":" << trade.quantity
           << ",\"aggressorSide\":" << quoted(side_name(trade.aggressor_side)) << "}";
    return output.str();
}

struct Event {
    std::uint64_t sequence;
    std::string type;
    std::string response;
};

class Adapter {
public:
    explicit Adapter(std::size_t capacity)
        : book_(capacity) {}

    std::string handle(const Fields& fields) {
        const std::string& type = required(fields, "type");
        const std::string request_id = fields.contains("requestId") ? fields.at("requestId") : "";
        if (type == "add_order") return add(fields, request_id);
        if (type == "cancel_order") return cancel(fields, request_id);
        if (type == "snapshot") return snapshot(fields, request_id);
        if (type == "order") return order(fields, request_id);
        if (type == "recent_events") return recent_events(fields, request_id);
        if (type == "health") return health(request_id);
        throw std::runtime_error("unsupported request type");
    }

private:
    std::string envelope(std::string_view request_id, std::string_view body) const {
        return "{\"requestId\":" + quoted(request_id) + "," + std::string(body) + "}";
    }

    std::string add(const Fields& fields, const std::string& request_id) {
        const lob::OrderId id = unsigned_value(fields, "orderId");
        const std::string& side_value = required(fields, "side");
        const lob::Side side = side_value == "BUY" ? lob::Side::BUY :
                               side_value == "SELL" ? lob::Side::SELL :
                               throw std::runtime_error("side must be BUY or SELL");
        const lob::Price price = unsigned_value(fields, "price");
        const std::uint64_t quantity_value = unsigned_value(fields, "quantity");
        if (quantity_value == 0 || quantity_value > std::numeric_limits<lob::Quantity>::max()) {
            throw std::runtime_error("quantity must fit in uint32 and be positive");
        }
        const lob::Quantity quantity = static_cast<lob::Quantity>(quantity_value);
        if (price == 0 || price >= lob::MAX_PRICE_TICKS) {
            throw std::runtime_error("price must be between 1 and MAX_PRICE_TICKS - 1");
        }
        book_.add_order(id, side, price, quantity);
        const auto& report = book_.last_report();
        std::ostringstream body;
        body << "\"ok\":" << (report.ok() ? "true" : "false")
             << ",\"type\":\"execution_report\",\"report\":" << report_json(report)
             << ",\"trades\":[";
        for (std::size_t i = 0; i < book_.last_trades().size(); ++i) {
            if (i != 0) body << ',';
            body << trade_json(book_.last_trades()[i]);
        }
        body << "]";
        return record("execution_report", envelope(request_id, body.str()));
    }

    std::string cancel(const Fields& fields, const std::string& request_id) {
        const lob::OrderId id = unsigned_value(fields, "orderId");
        book_.cancel_order(id);
        const auto& report = book_.last_report();
        std::ostringstream body;
        body << "\"ok\":" << (report.ok() ? "true" : "false")
             << ",\"type\":\"execution_report\",\"report\":" << report_json(report)
             << ",\"trades\":[]";
        return record("execution_report", envelope(request_id, body.str()));
    }

    std::string snapshot(const Fields& fields, const std::string& request_id) const {
        const std::size_t depth = fields.contains("depth")
            ? bounded_limit(fields, "depth", 1000)
            : 20;
        std::ostringstream body;
        body << "\"ok\":true,\"type\":\"snapshot\",\"bestBid\":";
        if (const auto* level = book_.best_bid(); level != nullptr) {
            body << "{\"price\":" << level->price()
                 << ",\"quantity\":" << level->total_volume()
                 << ",\"orders\":" << level->order_count() << "}";
        } else {
            body << "null";
        }
        body << ",\"bestAsk\":";
        if (const auto* level = book_.best_ask(); level != nullptr) {
            body << "{\"price\":" << level->price()
                 << ",\"quantity\":" << level->total_volume()
                 << ",\"orders\":" << level->order_count() << "}";
        } else {
            body << "null";
        }
        body << ",\"spread\":" << book_.spread() << ",\"bids\":[";
        std::size_t count = 0;
        for (lob::Price price = book_.best_bid_tick(); price > 0 && count < depth; --price) {
            const auto* level = book_.level_at(lob::Side::BUY, price);
            if (level != nullptr && !level->empty()) {
                if (count++ != 0) body << ',';
                body << "{\"price\":" << price << ",\"quantity\":" << level->total_volume()
                     << ",\"orders\":" << level->order_count() << "}";
            }
        }
        body << "],\"asks\":[";
        count = 0;
        for (lob::Price price = book_.best_ask_tick(); price < lob::MAX_PRICE_TICKS && count < depth; ++price) {
            const auto* level = book_.level_at(lob::Side::SELL, price);
            if (level != nullptr && !level->empty()) {
                if (count++ != 0) body << ',';
                body << "{\"price\":" << price << ",\"quantity\":" << level->total_volume()
                     << ",\"orders\":" << level->order_count() << "}";
            }
        }
        body << "]";
        return envelope(request_id, body.str());
    }

    std::string order(const Fields& fields, const std::string& request_id) const {
        const lob::OrderId id = unsigned_value(fields, "orderId");
        const auto* value = book_.find_order(id);
        std::ostringstream body;
        body << "\"ok\":" << (value != nullptr ? "true" : "false")
             << ",\"type\":\"order\",\"order\":";
        if (value == nullptr) {
            body << "null";
        } else {
            body << "{\"orderId\":" << value->order_id
                 << ",\"side\":" << quoted(side_name(value->side))
                 << ",\"price\":" << value->price
                 << ",\"quantity\":" << value->quantity
                 << ",\"timestamp\":" << value->timestamp << "}";
        }
        return envelope(request_id, body.str());
    }

    std::string recent_events(const Fields& fields, const std::string& request_id) const {
        const std::size_t limit = fields.contains("limit")
            ? bounded_limit(fields, "limit", 1000)
            : 100;
        std::ostringstream body;
        body << "\"ok\":true,\"type\":\"recent_events\",\"events\":[";
        const std::size_t start = events_.size() > limit ? events_.size() - limit : 0;
        for (std::size_t i = start; i < events_.size(); ++i) {
            if (i != start) body << ',';
            body << events_[i].response;
        }
        body << "]";
        return envelope(request_id, body.str());
    }

    std::string health(const std::string& request_id) const {
        std::ostringstream body;
        body << "\"ok\":true,\"type\":\"health\",\"liveOrders\":"
             << book_.live_order_count() << ",\"availablePoolSlots\":"
             << book_.pool().available();
        return envelope(request_id, body.str());
    }

    std::string record(std::string type, std::string response) {
        const std::uint64_t sequence = ++sequence_;
        if (response.empty() || response.back() != '}') {
            throw std::runtime_error("internal event serialization error");
        }
        response.pop_back();
        response += ",\"sequence\":" + std::to_string(sequence) + "}";
        events_.push_back(Event{sequence, std::move(type), std::move(response)});
        if (events_.size() > 1000) events_.pop_front();
        return events_.back().response;
    }

    lob::OrderBook book_;
    std::deque<Event> events_;
    std::uint64_t sequence_ = 0;
};

std::string error_response(std::string_view request_id, std::string_view message) {
    return "{\"requestId\":" + quoted(request_id) +
           ",\"ok\":false,\"error\":{\"code\":\"INVALID_REQUEST\",\"message\":" +
           quoted(message) + "}}";
}

} // namespace

int main(int argc, char** argv) {
    std::size_t capacity = lob::OrderPool::kDefaultCapacity;
    if (argc == 2) {
        const std::string value(argv[1]);
        std::uint64_t parsed = 0;
        const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), parsed);
        if (error != std::errc{} || end != value.data() + value.size() || parsed == 0) {
            std::cerr << "invalid capacity\n";
            return 2;
        }
        capacity = static_cast<std::size_t>(parsed);
    } else if (argc > 2) {
        std::cerr << "usage: trademind_engine_adapter [capacity]\n";
        return 2;
    }

    try {
        Adapter adapter(capacity);
        std::string line;
        while (std::getline(std::cin, line)) {
            if (line.size() > 64 * 1024) {
                std::cout << error_response("", "request exceeds 64 KiB") << '\n' << std::flush;
                continue;
            }
            std::string request_id;
            try {
                const Fields fields = parse_object(line);
                if (fields.contains("requestId")) request_id = fields.at("requestId");
                std::cout << adapter.handle(fields) << '\n' << std::flush;
            } catch (const std::exception& error) {
                std::cout << error_response(request_id, error.what()) << '\n' << std::flush;
            }
        }
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}

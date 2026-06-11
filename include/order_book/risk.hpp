#pragma once

#include <cstdint>
#include <optional>

#include "order_book/strategy.hpp"
#include "order_book/types.hpp"

namespace ob {

// kQuantityScale: 1e-6 BTC units per Quantity unit (mirrors feed.hpp).
// Redeclared here to keep risk.hpp independent of feed.hpp.
inline constexpr std::int64_t kRiskQuantityScale = 1'000'000;

struct RiskConfig {
    std::int64_t max_position_qty = 0;  // max ABS btc_qty (qty-scale); 0 = unlimited
    std::int64_t max_notional     = 0;  // max ABS position notional in price-scale; 0 = unlimited
    std::int64_t max_drawdown     = 0;  // max equity drop from peak in price-scale; 0 = unlimited
};

class RiskManager {
public:
    explicit RiskManager(const RiskConfig& cfg) noexcept
        : cfg_(cfg), peak_equity_(0), halted_(false) {}

    // Update running peak equity; call each tick with current equity (price-scale).
    // Once drawdown exceeds max_drawdown, halted_ is set permanently.
    void observe_equity(std::int64_t equity) noexcept {
        if (equity > peak_equity_) {
            peak_equity_ = equity;
        }
        if (!halted_ && cfg_.max_drawdown > 0) {
            const std::int64_t drawdown = peak_equity_ - equity;
            if (drawdown > cfg_.max_drawdown) {
                halted_ = true;
            }
        }
    }

    // Returns true once drawdown has been breached — trading must stop.
    bool halted() const noexcept { return halted_; }

    // Gate a proposed action given current position and execution price (price-scale).
    // Returns the allowed action (possibly with reduced qty), or nullopt if fully rejected.
    // Semantics:
    //   - Returns nullopt if halted().
    //   - Buys  increase btc_qty; sells decrease btc_qty.
    //   - Clamps qty to the largest value that keeps abs(post_qty) <= max_position_qty
    //     AND abs(post_notional) <= max_notional (0 = unlimited for either limit).
    //   - Returns nullopt if the clamped qty is 0.
    [[nodiscard]] std::optional<StrategyAction>
    gate(const StrategyAction& proposed,
         std::int64_t cur_btc_qty,
         Price exec_px) const noexcept {
        if (halted_) {
            return std::nullopt;
        }

        // Direction factor: +1 for Buy, -1 for Sell.
        const std::int64_t dir = (proposed.side == Side::Buy) ? 1 : -1;

        // Maximum qty we can trade — start unconstrained.
        std::int64_t allowed_qty = static_cast<std::int64_t>(proposed.qty);

        // --- Position-qty limit ---
        if (cfg_.max_position_qty > 0) {
            const std::int64_t post_qty = cur_btc_qty + dir * allowed_qty;
            const std::int64_t abs_post = post_qty < 0 ? -post_qty : post_qty;
            if (abs_post > cfg_.max_position_qty) {
                // Room left before the limit is hit from the current position side.
                // post_qty = cur_btc_qty + dir * q  =>  |cur + dir*q| <= max
                // We want the largest q >= 0 such that abs(cur + dir*q) <= max.
                const std::int64_t room =
                    cfg_.max_position_qty - (cur_btc_qty * dir);  // dir * cur projected onto dir axis
                if (room <= 0) {
                    return std::nullopt;  // already at or beyond the limit in this direction
                }
                allowed_qty = room < allowed_qty ? room : allowed_qty;
            }
        }

        // --- Notional limit ---
        if (cfg_.max_notional > 0 && exec_px > 0) {
            const std::int64_t post_qty    = cur_btc_qty + dir * allowed_qty;
            const std::int64_t post_notional =
                (post_qty < 0 ? -post_qty : post_qty) *
                exec_px / kRiskQuantityScale;
            if (post_notional > cfg_.max_notional) {
                // Solve: abs(cur + dir*q) * px / scale <= max_notional
                // => abs(cur + dir*q) <= max_notional * scale / px
                const std::int64_t max_abs_qty =
                    cfg_.max_notional * kRiskQuantityScale / exec_px;
                // Room in the direction of the trade.
                const std::int64_t room = max_abs_qty - (cur_btc_qty * dir);
                if (room <= 0) {
                    return std::nullopt;
                }
                allowed_qty = room < allowed_qty ? room : allowed_qty;
            }
        }

        if (allowed_qty <= 0) {
            return std::nullopt;
        }

        return StrategyAction{proposed.side, static_cast<Quantity>(allowed_qty)};
    }

private:
    RiskConfig   cfg_;
    std::int64_t peak_equity_;
    bool         halted_;
};

}  // namespace ob

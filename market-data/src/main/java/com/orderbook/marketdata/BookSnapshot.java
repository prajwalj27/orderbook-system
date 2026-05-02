package com.orderbook.marketdata;

import lombok.AllArgsConstructor;
import lombok.Data;
import lombok.NoArgsConstructor;

import java.time.Instant;
import java.util.List;

@Data
@NoArgsConstructor
@AllArgsConstructor
public class BookSnapshot {
    private List<PriceLevel> bids;      // best (highest) first
    private List<PriceLevel> asks;      // best (lowest) first
    private Instant          updatedAt;

    public double bestBid() {
        return bids.isEmpty() ? 0.0 : bids.get(0).getPrice();
    }

    public double bestAsk() {
        return asks.isEmpty() ? 0.0 : asks.get(0).getPrice();
    }

    public double spread() {
        if (bids.isEmpty() || asks.isEmpty()) return 0.0;
        return bestAsk() - bestBid();
    }
}

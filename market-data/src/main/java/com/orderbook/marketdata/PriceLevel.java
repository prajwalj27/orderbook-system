package com.orderbook.marketdata;

import lombok.AllArgsConstructor;
import lombok.Data;

@Data
@AllArgsConstructor
public class PriceLevel {
    private double price;
    private int    quantity;
}

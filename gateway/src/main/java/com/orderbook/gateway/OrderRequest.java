package com.orderbook.gateway;

import lombok.Data;
import lombok.NoArgsConstructor;
import lombok.AllArgsConstructor;

@Data
@NoArgsConstructor
@AllArgsConstructor
public class OrderRequest {
    private String orderId;
    private String side;
    private String type;
    private Double price;
    private int    quantity;
}

package com.orderbook.gateway;

import lombok.Data;
import lombok.NoArgsConstructor;
import lombok.AllArgsConstructor;

@Data
@NoArgsConstructor
@AllArgsConstructor
public class OrderResponse {
    private String orderId;
    private String status;
    private int    cumQty;
    private int    leavesQty;
    private Double lastPx;
    private String message;

    public static OrderResponse rejected(String reason) {
        OrderResponse r = new OrderResponse();
        r.status  = "REJECTED";
        r.message = reason;
        return r;
    }
}

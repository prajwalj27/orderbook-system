package com.orderbook.consumer;

import jakarta.persistence.*;
import lombok.Data;
import lombok.NoArgsConstructor;

import java.time.Instant;

@Entity
@Table(name = "trades")
@Data
@NoArgsConstructor
public class Trade {

    @Id
    @GeneratedValue(strategy = GenerationType.IDENTITY)
    private Long id;

    @Column(nullable = false)
    private String buyOrderId;

    @Column(nullable = false)
    private String sellOrderId;

    @Column(nullable = false)
    private double price;

    @Column(nullable = false)
    private int quantity;

    private long engineTimestamp;   // ts field from engine (nanosecond epoch)

    @Column(nullable = false)
    private Instant receivedAt;

    public Trade(String buyOrderId, String sellOrderId, double price, int quantity, long engineTimestamp) {
        this.buyOrderId      = buyOrderId;
        this.sellOrderId     = sellOrderId;
        this.price           = price;
        this.quantity        = quantity;
        this.engineTimestamp = engineTimestamp;
        this.receivedAt      = Instant.now();
    }
}

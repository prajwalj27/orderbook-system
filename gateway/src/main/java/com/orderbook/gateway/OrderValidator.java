package com.orderbook.gateway;

import org.springframework.beans.factory.annotation.Autowired;
import org.springframework.beans.factory.annotation.Value;
import org.springframework.stereotype.Component;

import java.util.Optional;

@Component
public class OrderValidator {

    @Value("${risk.reference-price}")
    private double referencePrice;

    @Value("${risk.price-collar-pct}")
    private double collarPct;

    @Value("${risk.max-quantity}")
    private int maxQuantity;

    @Autowired
    private WalWriter walWriter;

    public Optional<String> validate(OrderRequest req) {

        // 1. required fields
        if (req.getOrderId() == null || req.getOrderId().isBlank())
            return Optional.of("orderId is required");

        if (req.getSide() == null || req.getSide().isBlank())
            return Optional.of("side is required");

        if (req.getType() == null || req.getType().isBlank())
            return Optional.of("type is required");

        if (req.getQuantity() <= 0)
            return Optional.of("quantity must be greater than zero");

        // 2. side validation
        String side = req.getSide().toUpperCase();
        if (!side.equals("BUY") && !side.equals("SELL"))
            return Optional.of("side must be BUY or SELL");

        // 3. price rules per order type
        String type = req.getType().toUpperCase();
        switch (type) {
            case "LIMIT", "IOC" -> {
                if (req.getPrice() == null)
                    return Optional.of("price is required for " + type + " orders");
            }
            case "MARKET" -> {
                if (req.getPrice() != null)
                    return Optional.of("price must not be set for MARKET orders");
            }
            default -> { return Optional.of("type must be LIMIT, MARKET, or IOC"); }
        }

        // 4. price collar — LIMIT and IOC only
        if (req.getPrice() != null) {
            double collar = referencePrice * (collarPct / 100.0);
            double lower  = referencePrice - collar;
            double upper  = referencePrice + collar;
            if (req.getPrice() < lower || req.getPrice() > upper)
                return Optional.of(String.format(
                    "price %.2f outside collar [%.2f, %.2f]",
                    req.getPrice(), lower, upper));
        }

        // 5. quantity limit
        if (req.getQuantity() > maxQuantity)
            return Optional.of("quantity " + req.getQuantity() + " exceeds max " + maxQuantity);

        // 6. duplicate order ID
        if (walWriter.contains(req.getOrderId()))
            return Optional.of("duplicate orderId: " + req.getOrderId());

        return Optional.empty();
    }
}

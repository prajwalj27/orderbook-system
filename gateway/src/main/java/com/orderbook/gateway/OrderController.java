package com.orderbook.gateway;

import org.springframework.beans.factory.annotation.Autowired;
import org.springframework.http.ResponseEntity;
import org.springframework.web.bind.annotation.*;

@RestController
@RequestMapping("/orders")
public class OrderController {

    @Autowired private OrderValidator validator;
    @Autowired private WalWriter       walWriter;
    @Autowired private EngineClient    engineClient;

    @PostMapping
    public ResponseEntity<OrderResponse> placeOrder(@RequestBody OrderRequest req) {

        // 1. validate
        var error = validator.validate(req);
        if (error.isPresent())
            return ResponseEntity.badRequest().body(OrderResponse.rejected(error.get()));

        // 2. build FIX, write to WAL before touching the engine
        String fix = engineClient.toFix(req);
        try {
            walWriter.write(fix);
        } catch (Exception e) {
            return ResponseEntity.internalServerError()
                    .body(OrderResponse.rejected("WAL write failed: " + e.getMessage()));
        }

        // 3. forward to engine, return execution report
        try {
            return ResponseEntity.ok(engineClient.sendRaw(fix));
        } catch (Exception e) {
            return ResponseEntity.internalServerError()
                    .body(OrderResponse.rejected("Engine error: " + e.getMessage()));
        }
    }

    @DeleteMapping("/{orderId}")
    public ResponseEntity<OrderResponse> cancelOrder(@PathVariable String orderId) {

        String fix = "35=F|11=" + orderId;
        try {
            walWriter.write(fix);
        } catch (Exception e) {
            return ResponseEntity.internalServerError()
                    .body(OrderResponse.rejected("WAL write failed: " + e.getMessage()));
        }

        try {
            return ResponseEntity.ok(engineClient.cancel(orderId));
        } catch (Exception e) {
            return ResponseEntity.internalServerError()
                    .body(OrderResponse.rejected("Engine error: " + e.getMessage()));
        }
    }
}

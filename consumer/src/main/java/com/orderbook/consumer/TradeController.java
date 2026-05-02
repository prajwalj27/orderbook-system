package com.orderbook.consumer;

import org.springframework.http.ResponseEntity;
import org.springframework.web.bind.annotation.*;

import java.util.List;

@RestController
@RequestMapping("/trades")
public class TradeController {

    private final TradeRepository tradeRepository;

    public TradeController(TradeRepository tradeRepository) {
        this.tradeRepository = tradeRepository;
    }

    @GetMapping
    public List<Trade> getAll() {
        return tradeRepository.findAll();
    }

    @GetMapping("/{orderId}")
    public ResponseEntity<List<Trade>> getByOrderId(@PathVariable String orderId) {
        var trades = tradeRepository.findByOrderId(orderId);
        if (trades.isEmpty()) return ResponseEntity.notFound().build();
        return ResponseEntity.ok(trades);
    }
}

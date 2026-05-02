package com.orderbook.consumer;

import org.springframework.data.jpa.repository.JpaRepository;
import org.springframework.data.jpa.repository.Query;
import org.springframework.data.repository.query.Param;

import java.util.List;

public interface TradeRepository extends JpaRepository<Trade, Long> {

    List<Trade> findByBuyOrderId(String orderId);

    List<Trade> findBySellOrderId(String orderId);

    // trades where the given order ID appears on either side
    @Query("SELECT t FROM Trade t WHERE t.buyOrderId = :id OR t.sellOrderId = :id")
    List<Trade> findByOrderId(@Param("id") String orderId);
}

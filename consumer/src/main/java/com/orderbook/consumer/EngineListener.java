package com.orderbook.consumer;

import jakarta.annotation.PostConstruct;
import jakarta.annotation.PreDestroy;
import org.springframework.beans.factory.annotation.Value;
import org.springframework.stereotype.Component;

import java.io.*;
import java.net.Socket;
import java.util.HashMap;
import java.util.Map;

@Component
public class EngineListener {

    @Value("${engine.host}")
    private String engineHost;

    @Value("${engine.port.consumer:7002}")
    private int enginePort;

    private final TradeRepository tradeRepository;

    private Socket          socket;
    private volatile boolean running = false;
    private Thread          listenerThread;

    public EngineListener(TradeRepository tradeRepository) {
        this.tradeRepository = tradeRepository;
    }

    @PostConstruct
    public void start() {
        running = true;
        listenerThread = new Thread(this::listenLoop, "engine-listener");
        listenerThread.setDaemon(true);
        listenerThread.start();
        System.out.println("EngineListener started — connecting to " + engineHost + ":" + enginePort);
    }

    @PreDestroy
    public void stop() {
        running = false;
        try {
            if (socket != null && !socket.isClosed()) socket.close();
        } catch (IOException ignored) {}
    }

    // ── private ───────────────────────────────────────────────────────────

    private void listenLoop() {
        while (running) {
            try {
                socket = new Socket(engineHost, enginePort);
                System.out.println("EngineListener connected to engine on port " + enginePort);

                BufferedReader in = new BufferedReader(new InputStreamReader(socket.getInputStream()));
                String line;
                while (running && (line = in.readLine()) != null) {
                    handleMessage(line.trim());
                }

            } catch (IOException e) {
                if (!running) break;
                System.err.println("EngineListener lost connection — retrying in 3s: " + e.getMessage());
                try { Thread.sleep(3000); } catch (InterruptedException ie) { Thread.currentThread().interrupt(); }
            }
        }
    }

    private void handleMessage(String raw) {
        if (raw.isEmpty() || !raw.startsWith("TRADE")) return;

        // Format: TRADE|buyOrderId=X|sellOrderId=Y|price=Z|qty=N|ts=T
        Map<String, String> fields = parseKeyValue(raw);

        String buyOrderId  = fields.get("buyOrderId");
        String sellOrderId = fields.get("sellOrderId");
        double price       = Double.parseDouble(fields.getOrDefault("price", "0"));
        int    qty         = Integer.parseInt(fields.getOrDefault("qty",   "0"));
        long   ts          = Long.parseLong(fields.getOrDefault("ts",    "0"));

        if (buyOrderId == null || sellOrderId == null) return;

        Trade trade = new Trade(buyOrderId, sellOrderId, price, qty, ts);
        tradeRepository.save(trade);
        System.out.printf("Saved trade — buy=%s sell=%s price=%.2f qty=%d%n",
                buyOrderId, sellOrderId, price, qty);
    }

    private Map<String, String> parseKeyValue(String raw) {
        Map<String, String> fields = new HashMap<>();
        for (String part : raw.split("\\|")) {
            int eq = part.indexOf('=');
            if (eq != -1) fields.put(part.substring(0, eq), part.substring(eq + 1));
        }
        return fields;
    }
}

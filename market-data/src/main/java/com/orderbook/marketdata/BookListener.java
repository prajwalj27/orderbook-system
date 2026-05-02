package com.orderbook.marketdata;

import jakarta.annotation.PostConstruct;
import jakarta.annotation.PreDestroy;
import org.springframework.beans.factory.annotation.Value;
import org.springframework.stereotype.Component;

import java.io.*;
import java.net.Socket;
import java.time.Instant;
import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.atomic.AtomicReference;

@Component
public class BookListener {

    @Value("${engine.host}")
    private String engineHost;

    @Value("${engine.port.marketdata:7003}")
    private int enginePort;

    private final AtomicReference<BookSnapshot> latest = new AtomicReference<>(emptySnapshot());

    private Socket          socket;
    private volatile boolean running = false;

    @PostConstruct
    public void start() {
        running = true;
        Thread t = new Thread(this::listenLoop, "book-listener");
        t.setDaemon(true);
        t.start();
        System.out.println("BookListener started — connecting to " + engineHost + ":" + enginePort);
    }

    @PreDestroy
    public void stop() {
        running = false;
        try { if (socket != null) socket.close(); } catch (IOException ignored) {}
    }

    public BookSnapshot getLatest() {
        return latest.get();
    }

    // ── private ───────────────────────────────────────────────────────────

    private void listenLoop() {
        while (running) {
            try {
                socket = new Socket(engineHost, enginePort);
                System.out.println("BookListener connected to engine on port " + enginePort);

                BufferedReader in = new BufferedReader(new InputStreamReader(socket.getInputStream()));
                String line;
                while (running && (line = in.readLine()) != null) {
                    BookSnapshot snap = parseSnapshot(line.trim());
                    if (snap != null) latest.set(snap);
                }

            } catch (IOException e) {
                if (!running) break;
                System.err.println("BookListener lost connection — retrying in 3s: " + e.getMessage());
                try { Thread.sleep(3000); } catch (InterruptedException ie) { Thread.currentThread().interrupt(); }
            }
        }
    }

    private BookSnapshot parseSnapshot(String raw) {
        if (!raw.startsWith("BOOK")) return null;

        // Format: BOOK|bids=150.00x100,149.50x80|asks=150.50x60,151.00x40
        String bidsStr = extractField(raw, "bids");
        String asksStr = extractField(raw, "asks");

        return new BookSnapshot(parseLevels(bidsStr), parseLevels(asksStr), Instant.now());
    }

    private List<PriceLevel> parseLevels(String raw) {
        List<PriceLevel> levels = new ArrayList<>();
        if (raw.isEmpty()) return levels;
        for (String part : raw.split(",")) {
            String[] tokens = part.split("x");
            if (tokens.length == 2) {
                double price = Double.parseDouble(tokens[0]);
                int    qty   = Integer.parseInt(tokens[1]);
                levels.add(new PriceLevel(price, qty));
            }
        }
        return levels;
    }

    private String extractField(String raw, String key) {
        String search = "|" + key + "=";
        int start = raw.indexOf(search);
        if (start == -1) return "";
        start += search.length();
        int end = raw.indexOf("|", start);
        return end == -1 ? raw.substring(start) : raw.substring(start, end);
    }

    private static BookSnapshot emptySnapshot() {
        return new BookSnapshot(List.of(), List.of(), Instant.now());
    }
}

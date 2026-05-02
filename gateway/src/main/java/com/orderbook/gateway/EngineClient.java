package com.orderbook.gateway;

import jakarta.annotation.PostConstruct;
import jakarta.annotation.PreDestroy;
import org.springframework.beans.factory.annotation.Value;
import org.springframework.stereotype.Component;

import java.io.*;
import java.net.Socket;
import java.util.HashMap;
import java.util.Map;

@Component
public class EngineClient {

    @Value("${engine.host}")
    private String engineHost;

    @Value("${engine.port}")
    private int enginePort;

    private Socket         socket;
    private BufferedWriter out;
    private BufferedReader in;

    @PostConstruct
    public void connect() throws IOException {
        try {
            socket = new Socket(engineHost, enginePort);
            out = new BufferedWriter(new OutputStreamWriter(socket.getOutputStream()));
            in  = new BufferedReader(new InputStreamReader(socket.getInputStream()));
            System.out.println("EngineClient connected to " + engineHost + ":" + enginePort);
        } catch (IOException e) {
            throw new IllegalStateException(
                "Cannot connect to engine at " + engineHost + ":" + enginePort +
                " — is the engine running?", e);
        }
    }

    public synchronized OrderResponse sendRaw(String fixMessage) throws IOException {
        out.write(fixMessage + "\n");
        out.flush();
        return parseResponse(in.readLine());
    }

    public synchronized OrderResponse send(OrderRequest req) throws IOException {
        return sendRaw(toFix(req));
    }

    public synchronized OrderResponse cancel(String orderId) throws IOException {
        out.write("35=F|11=" + orderId + "\n");
        out.flush();
        return parseResponse(in.readLine());
    }

    @PreDestroy
    public void disconnect() throws IOException {
        if (socket != null && !socket.isClosed()) socket.close();
    }

    // ── private helpers ───────────────────────────────────────────────────

    String toFix(OrderRequest req) {          // package-private — used by OrderController
        StringBuilder sb = new StringBuilder();
        sb.append("35=D");
        sb.append("|11=").append(req.getOrderId());
        sb.append("|54=").append(req.getSide().equalsIgnoreCase("BUY") ? "1" : "2");
        sb.append("|38=").append(req.getQuantity());

        if (req.getPrice() != null)
            sb.append("|44=").append(String.format("%.2f", req.getPrice()));

        switch (req.getType().toUpperCase()) {
            case "MARKET" -> sb.append("|40=1");
            case "IOC"    -> sb.append("|40=3");
        }

        return sb.toString();
    }

    private OrderResponse parseResponse(String raw) {
        if (raw == null) return null;

        Map<String, String> tags = new HashMap<>();
        for (String part : raw.split("\\|")) {
            int eq = part.indexOf('=');
            if (eq != -1) tags.put(part.substring(0, eq), part.substring(eq + 1));
        }

        String status = switch (tags.getOrDefault("39", "")) {
            case "0" -> "NEW";
            case "1" -> "PARTIAL";
            case "2" -> "FILLED";
            case "4" -> "CANCELLED";
            default  -> "UNKNOWN";
        };

        int    cumQty    = Integer.parseInt(tags.getOrDefault("14",  "0"));
        int    leavesQty = Integer.parseInt(tags.getOrDefault("151", "0"));
        Double lastPx    = tags.containsKey("31") ? Double.parseDouble(tags.get("31")) : null;

        return new OrderResponse(tags.get("11"), status, cumQty, leavesQty, lastPx, null);
    }
}

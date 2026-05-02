package com.orderbook.client;

import java.net.URI;
import java.net.http.HttpClient;
import java.net.http.HttpRequest;
import java.net.http.HttpResponse;
import java.util.Random;

public class ClientSimulator {

    private static final String GATEWAY_URL    = "http://localhost:8080/orders";
    private static final double REFERENCE_PRICE = 150.00;
    private static final double PRICE_SPREAD    = 5.00;   // ± around reference
    private static final int    ORDER_COUNT     = 20;
    private static final int    DELAY_MS        = 300;

    private static final String[] SIDES = {"BUY", "SELL"};
    // weighted: LIMIT appears 3x more often than MARKET or IOC
    private static final String[] TYPES = {"LIMIT", "LIMIT", "LIMIT", "MARKET", "IOC"};

    public static void main(String[] args) throws Exception {
        HttpClient client = HttpClient.newHttpClient();
        Random     random = new Random();

        System.out.println("Orderbook Client Simulator v1.0");
        System.out.println("Gateway : " + GATEWAY_URL);
        System.out.printf("Sending  %d orders (ref price %.2f ± %.2f)%n%n",
                ORDER_COUNT, REFERENCE_PRICE, PRICE_SPREAD);

        int filled = 0, rejected = 0;

        for (int i = 1; i <= ORDER_COUNT; i++) {
            String orderId  = String.format("SIM-%04d", i);
            String side     = SIDES[random.nextInt(SIDES.length)];
            String type     = TYPES[random.nextInt(TYPES.length)];
            int    quantity = (random.nextInt(10) + 1) * 10;   // 10, 20, … 100

            String body = buildJson(orderId, side, type, quantity, random);

            HttpRequest request = HttpRequest.newBuilder()
                    .uri(URI.create(GATEWAY_URL))
                    .header("Content-Type", "application/json")
                    .POST(HttpRequest.BodyPublishers.ofString(body))
                    .build();

            HttpResponse<String> response = client.send(request, HttpResponse.BodyHandlers.ofString());

            String status = extractField(response.body(), "status");
            System.out.printf("[%s] %-4s %-6s qty=%-4d → %s%n",
                    orderId, side, type, quantity, response.body());

            if ("REJECTED".equals(status)) rejected++;
            else                            filled++;

            Thread.sleep(DELAY_MS);
        }

        System.out.printf("%nDone. %d accepted, %d rejected.%n", filled, rejected);
        System.out.println("Check http://localhost:8081/trades for filled orders.");
    }

    // ── helpers ───────────────────────────────────────────────────────────────

    private static String buildJson(String orderId, String side, String type,
                                    int quantity, Random random) {
        if ("MARKET".equals(type)) {
            return String.format(
                    "{\"orderId\":\"%s\",\"side\":\"%s\",\"quantity\":%d,\"type\":\"%s\"}",
                    orderId, side, quantity, type);
        }
        double price = REFERENCE_PRICE + (random.nextDouble() * PRICE_SPREAD * 2 - PRICE_SPREAD);
        price = Math.round(price * 100.0) / 100.0;
        return String.format(
                "{\"orderId\":\"%s\",\"side\":\"%s\",\"quantity\":%d,\"price\":%.2f,\"type\":\"%s\"}",
                orderId, side, quantity, price, type);
    }

    private static String extractField(String json, String key) {
        String search = "\"" + key + "\":\"";
        int start = json.indexOf(search);
        if (start == -1) return "";
        start += search.length();
        int end = json.indexOf("\"", start);
        return end == -1 ? "" : json.substring(start, end);
    }
}

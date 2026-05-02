package com.orderbook.gateway;

import jakarta.annotation.PostConstruct;
import org.springframework.beans.factory.annotation.Value;
import org.springframework.stereotype.Component;

import java.io.*;
import java.nio.file.*;
import java.util.HashSet;
import java.util.Optional;
import java.util.Set;

@Component
public class WalWriter {

    @Value("${wal.path}")
    private String walPath;

    private final Set<String> seenIds = new HashSet<>();

    @PostConstruct
    public void init() throws IOException {
        Path path = Paths.get(walPath);
        Files.createDirectories(path.getParent());

        if (Files.exists(path)) {
            try (BufferedReader reader = Files.newBufferedReader(path)) {
                String line;
                while ((line = reader.readLine()) != null) {
                    extractOrderId(line).ifPresent(seenIds::add);
                }
            }
            System.out.println("WAL loaded — " + seenIds.size() + " existing order(s) found.");
        }
    }

    public synchronized void write(String fixMessage) throws IOException {
        long timestamp = System.currentTimeMillis() * 1_000_000L;
        String entry = timestamp + "|" + fixMessage;

        try (BufferedWriter writer = new BufferedWriter(new FileWriter(walPath, true))) {
            writer.write(entry);
            writer.newLine();
            writer.flush();
        }

        extractOrderId(fixMessage).ifPresent(seenIds::add);
    }

    public boolean contains(String orderId) {
        return seenIds.contains(orderId);
    }

    private Optional<String> extractOrderId(String line) {
        for (String part : line.split("\\|")) {
            if (part.startsWith("11=")) {
                return Optional.of(part.substring(3));
            }
        }
        return Optional.empty();
    }
}

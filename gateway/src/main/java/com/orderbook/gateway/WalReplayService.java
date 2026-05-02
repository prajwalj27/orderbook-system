package com.orderbook.gateway;

import jakarta.annotation.PostConstruct;
import org.springframework.beans.factory.annotation.Autowired;
import org.springframework.beans.factory.annotation.Value;
import org.springframework.stereotype.Component;

import java.io.*;
import java.nio.file.*;

@Component
public class WalReplayService {

    @Value("${wal.path}")
    private String walPath;

    @Autowired private EngineClient engineClient;

    @PostConstruct
    public void replay() {
        Path path = Paths.get(walPath);

        if (!Files.exists(path)) {
            System.out.println("WalReplayService — no WAL found, starting fresh.");
            return;
        }

        int replayed = 0;

        try (BufferedReader reader = Files.newBufferedReader(path)) {
            String line;
            while ((line = reader.readLine()) != null) {
                if (line.isBlank()) continue;

                // WAL format: TIMESTAMP_NS|FIX_MESSAGE — strip the timestamp
                int sep = line.indexOf('|');
                if (sep == -1) continue;

                String fixMessage = line.substring(sep + 1);
                engineClient.sendRaw(fixMessage);
                replayed++;
            }
        } catch (IOException e) {
            throw new IllegalStateException(
                "WalReplayService — failed to read WAL: " + e.getMessage(), e);
        }

        System.out.println("WalReplayService — replayed " + replayed +
                           " order(s). Gateway ready.");
    }
}

package com.orderbook.marketdata;

import org.springframework.web.bind.annotation.GetMapping;
import org.springframework.web.bind.annotation.RequestMapping;
import org.springframework.web.bind.annotation.RestController;

@RestController
@RequestMapping("/book")
public class BookController {

    private final BookListener bookListener;

    public BookController(BookListener bookListener) {
        this.bookListener = bookListener;
    }

    @GetMapping
    public BookSnapshot getBook() {
        return bookListener.getLatest();
    }
}

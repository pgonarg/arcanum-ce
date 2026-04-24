// arcanum-relay: TCP reverse proxy for Arcanum CE multiplayer.
//
// Usage:
//   arcanum-relay -host 192.168.1.5:12345
//   arcanum-relay -host 192.168.1.5:12345 -listen :19735 -max-guests 4
//
// The Arcanum host game still runs normally (binds port 12345 on its machine).
// This relay sits in front: guests connect to the relay's public address, and
// the relay opens a fresh TCP connection to the host on behalf of each guest.
// The host sees exactly the same traffic it would see from a direct connection.
//
// Wire protocol (from network.c):
//   Each message: [uint32_t LE length][payload bytes]
//   Max message size: 16384 bytes
//   The relay forwards raw bytes — no protocol parsing needed.
//
// Deployment scenarios:
//   A) Relay on same machine as host:
//        arcanum-relay -host 127.0.0.1:12345 -listen :19735
//        Guests connect to host-machine-public-ip:19735
//
//   B) Relay on VPS, host on LAN visible to VPS (e.g. same VPN):
//        arcanum-relay -host host-vpn-ip:12345 -listen :19735
//        Guests connect to vps-public-ip:19735
//
//   C) Relay on VPS, host also on VPS (dedicated headless server, future):
//        arcanum-relay -host 127.0.0.1:12345 -listen :19735

package main

import (
	"flag"
	"fmt"
	"io"
	"log"
	"net"
	"sync"
	"sync/atomic"
	"time"
)

var (
	activeGuests int64
)

func main() {
	hostAddr  := flag.String("host", "", "Arcanum host address, e.g. 192.168.1.5:12345 (required)")
	listenAddr := flag.String("listen", ":19735", "Address:port for guests to connect to")
	maxGuests  := flag.Int("max-guests", 7, "Maximum simultaneous guests (Arcanum supports up to 8 total)")
	flag.Parse()

	if *hostAddr == "" {
		log.Fatal("arcanum-relay: -host is required\n\nUsage: arcanum-relay -host <ip:port> [-listen <addr:port>] [-max-guests N]")
	}

	ln, err := net.Listen("tcp", *listenAddr)
	if err != nil {
		log.Fatalf("listen %s: %v", *listenAddr, err)
	}

	log.Printf("arcanum-relay started")
	log.Printf("  listening on  : %s", *listenAddr)
	log.Printf("  forwarding to : %s", *hostAddr)
	log.Printf("  max guests    : %d", *maxGuests)

	sem := make(chan struct{}, *maxGuests)

	for {
		conn, err := ln.Accept()
		if err != nil {
			log.Printf("accept: %v", err)
			continue
		}

		select {
		case sem <- struct{}{}:
			go func() {
				defer func() { <-sem }()
				tunnel(conn, *hostAddr)
			}()
		default:
			log.Printf("[%s] rejected: guest limit (%d) reached", conn.RemoteAddr(), *maxGuests)
			conn.Close()
		}
	}
}

// tunnel proxies bytes bidirectionally between a guest connection and a fresh
// connection to the Arcanum host. It logs byte counts and any errors on close.
func tunnel(guest net.Conn, hostAddr string) {
	guestStr := guest.RemoteAddr().String()
	n := atomic.AddInt64(&activeGuests, 1)

	defer func() {
		guest.Close()
		remaining := atomic.AddInt64(&activeGuests, -1)
		log.Printf("[%s] disconnected (%d active)", guestStr, remaining)
	}()

	// Dial the host, retrying briefly to tolerate a race between game start
	// and the guest's first connection attempt.
	var host net.Conn
	var err error
	for attempt := 0; attempt < 5; attempt++ {
		host, err = net.DialTimeout("tcp", hostAddr, 3*time.Second)
		if err == nil {
			break
		}
		if attempt < 4 {
			log.Printf("[%s] host unreachable (attempt %d/5): %v", guestStr, attempt+1, err)
			time.Sleep(time.Duration(attempt+1) * 500 * time.Millisecond)
		}
	}
	if err != nil {
		log.Printf("[%s] cannot reach host %s after 5 attempts: %v", guestStr, hostAddr, err)
		return
	}
	defer host.Close()

	log.Printf("[%s] tunnel open (%d active)", guestStr, n)

	// Forward bytes in both directions concurrently.
	// When either direction closes, signal the other by closing the write end.
	var wg sync.WaitGroup
	wg.Add(2)

	forward := func(dst, src net.Conn, label string) {
		defer wg.Done()
		bytes, err := io.Copy(dst, src)
		if err != nil {
			log.Printf("[%s] %s: %v (after %s)", guestStr, label, err, fmtBytes(bytes))
		} else {
			log.Printf("[%s] %s: EOF (%s forwarded)", guestStr, label, fmtBytes(bytes))
		}
		// Close the write side so the peer's io.Copy sees EOF and exits cleanly.
		if tc, ok := dst.(*net.TCPConn); ok {
			tc.CloseWrite()
		} else {
			dst.Close()
		}
	}

	go forward(host, guest, "guest→host")
	go forward(guest, host, "host→guest")
	wg.Wait()
}

func fmtBytes(n int64) string {
	switch {
	case n >= 1<<20:
		return fmt.Sprintf("%.1f MiB", float64(n)/(1<<20))
	case n >= 1<<10:
		return fmt.Sprintf("%.1f KiB", float64(n)/(1<<10))
	default:
		return fmt.Sprintf("%d B", n)
	}
}

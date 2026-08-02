package simpledb;

import simpledb.common.Database;
import simpledb.optimizer.TableStats;

import java.io.BufferedReader;
import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.io.InputStreamReader;
import java.io.PrintStream;
import java.nio.charset.StandardCharsets;

/**
 * Non-interactive driver for the SimpleDB SQL parser, meant to be run as a
 * child process communicating over stdin/stdout pipes (e.g. from a C
 * daemon). Unlike {@link Parser#start}, this never touches jline/ConsoleReader,
 * so it behaves the same whether stdin is a TTY or a pipe.
 *
 * Protocol: one SQL statement per line on stdin (terminated with ';').
 * After each statement, all output produced by the parser is flushed to
 * stdout followed by a single marker line: "<<END ok>>" or "<<END error>>".
 * On startup, "<<READY>>" is printed once the catalog is loaded. On EOF,
 * all dirty pages are flushed and the process exits cleanly.
 *
 * Note: {@link Parser#processNextStatement} catches every failure it can hit
 * (parse errors, aborted transactions, unsupported statements) internally
 * and only ever prints a message -- it never lets the exception propagate.
 * So success/failure can't be told apart by catching an exception here.
 * Instead, each statement's stdout is captured and scanned for the failure
 * messages Parser prints, and stderr is captured too: some Parser failure
 * paths (e.g. its IOException/DbException handler) only print a stack trace
 * to stderr, so any stderr output also means the statement failed. Captured
 * stderr is forwarded into the response body, which additionally means the
 * parent never needs to drain this process's stderr pipe.
 */
public class PipeRunner {

    /**
     * Failure messages Parser prints to stdout, anchored to the start of a
     * line so tab-separated result rows containing similar text can't
     * trigger them.
     */
    private static final String[] ERROR_LINE_PREFIXES = {
        "Invalid SQL expression",
        "Can't parse ",
    };

    public static void main(String[] args) throws IOException {
        if (args.length != 1) {
            System.err.println("Usage: PipeRunner catalogFile");
            System.exit(1);
        }

        Database.getCatalog().loadSchema(args[0]);
        TableStats.computeStatistics();

        PrintStream realOut = System.out;
        PrintStream realErr = System.err;
        PrintStream out = new PrintStream(realOut, true, "UTF-8");
        out.println("<<READY>>");
        out.flush();

        Parser parser = new Parser();
        BufferedReader in = new BufferedReader(
                new InputStreamReader(System.in, StandardCharsets.UTF_8));

        String line;
        while ((line = in.readLine()) != null) {
            line = line.trim();
            if (line.isEmpty()) {
                continue;
            }

            ByteArrayOutputStream outBuf = new ByteArrayOutputStream();
            ByteArrayOutputStream errBuf = new ByteArrayOutputStream();
            PrintStream capture = new PrintStream(outBuf, true, "UTF-8");
            System.setOut(capture);
            System.setErr(new PrintStream(errBuf, true, "UTF-8"));
            boolean threw = false;
            try {
                parser.processNextStatement(line);
            } catch (Exception e) {
                capture.println("ERROR: " + e.getMessage());
                threw = true;
            } finally {
                System.setOut(realOut);
                System.setErr(realErr);
            }

            String capturedOut = outBuf.toString("UTF-8");
            String capturedErr = errBuf.toString("UTF-8");
            out.print(capturedOut);
            if (!capturedOut.isEmpty() && !capturedOut.endsWith("\n")) {
                out.println();
            }
            out.print(capturedErr);
            if (!capturedErr.isEmpty() && !capturedErr.endsWith("\n")) {
                out.println();
            }
            boolean isError = threw || !capturedErr.isEmpty()
                    || hasErrorLine(capturedOut);
            out.println(isError ? "<<END error>>" : "<<END ok>>");
            out.flush();
        }

        Database.getBufferPool().flushAllPages();
        System.exit(0);
    }

    private static boolean hasErrorLine(String text) {
        for (String lineOut : text.split("\n", -1)) {
            for (String prefix : ERROR_LINE_PREFIXES) {
                if (lineOut.startsWith(prefix)) {
                    return true;
                }
            }
        }
        return false;
    }
}

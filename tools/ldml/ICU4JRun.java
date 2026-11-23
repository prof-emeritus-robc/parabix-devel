//  Part of the Parabix Project, under the Open Software License 3.0.
//  SPDX-License-Identifier: OSL-3.0

//  ICU4JRun: apply transforms with ICU4J as xlit --use-ICU applies them with
//  ICU4C (tools/ldml/icu_transform.cpp), for tools/ldml/icu4j_compare.py.
//
//  java -cp icu4j.jar:<dir> ICU4JRun <commands file>
//
//  Each line of the commands file is a command, its fields separated by tabs:
//      REG    id  reverse(0|1)  rules-file
//          make a transform from the rules (createFromRules) in the direction
//          and register it under the id (registerInstance); a failure is
//          ignored, as xlit ignores it (ICU's own transform of the id applies)
//      RULES  name  reverse(0|1)  rules-file  input  output
//          make a transform from the rules and apply it to each line of the
//          input, as a text of its own, writing a line of output for each
//      NAME   name  input  output
//          the same with ICU's own transform of a name (getInstance)
//  A transform that cannot be made writes the reason to output + ".err".

import com.ibm.icu.text.Transliterator;

import java.io.BufferedWriter;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.Paths;
import java.util.List;

public class ICU4JRun {
    static String read(String path) throws IOException {
        return new String(Files.readAllBytes(Paths.get(path)), StandardCharsets.UTF_8);
    }

    static int dir(String reverse) {
        return reverse.equals("1") ? Transliterator.REVERSE : Transliterator.FORWARD;
    }

    static void apply(Transliterator t, String input, String output) throws IOException {
        String text = read(input);
        String[] lines = text.split("\n", -1);
        int n = lines.length;
        if (n > 0 && lines[n - 1].isEmpty()) n--;   // (the final line end)
        try (BufferedWriter w = Files.newBufferedWriter(Paths.get(output), StandardCharsets.UTF_8)) {
            for (int i = 0; i < n; i++) {
                w.write(t.transliterate(lines[i]));
                w.write('\n');
            }
        }
    }

    static void fail(String output, Throwable e) throws IOException {
        Files.write(Paths.get(output + ".err"), (e.getClass().getSimpleName() + ": " + e.getMessage() + "\n")
                    .getBytes(StandardCharsets.UTF_8));
    }

    public static void main(String[] args) throws IOException {
        List<String> commands = Files.readAllLines(Paths.get(args[0]), StandardCharsets.UTF_8);
        for (String c : commands) {
            if (c.isEmpty()) continue;
            String[] f = c.split("\t", -1);
            switch (f[0]) {
                case "REG":
                    try {
                        Transliterator.registerInstance(Transliterator.createFromRules(f[1], read(f[3]), dir(f[2])));
                    } catch (RuntimeException e) {
                        // (as xlit: ICU's own transform of the id applies)
                    }
                    break;
                case "RULES":
                    try {
                        apply(Transliterator.createFromRules(f[1], read(f[3]), dir(f[2])), f[4], f[5]);
                    } catch (RuntimeException e) {
                        fail(f[5], e);
                    }
                    break;
                case "NAME":
                    try {
                        apply(Transliterator.getInstance(f[1]), f[2], f[3]);
                    } catch (RuntimeException e) {
                        fail(f[3], e);
                    }
                    break;
                default:
                    throw new IllegalArgumentException("unknown command " + f[0]);
            }
            System.out.println(f[1]);
            System.out.flush();
        }
    }
}

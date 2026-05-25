#!/usr/bin/env python3
import os
import glob
import subprocess
import sys
import time

def print_color(text, color):
    colors = {
        "green": "\033[92m",
        "red": "\033[91m",
        "yellow": "\033[93m",
        "reset": "\033[0m"
    }
    print(f"{colors.get(color, '')}{text}{colors['reset']}")

def main():
    start_time = time.time()
    
    # Exclude files that are just modules/imports and not meant to be run directly
    exclude_files = [
        'tests/modules/math.tr',
        'tests/modules/simple.tr',
        'tests/modules/simple2.tr'
    ]
    
    test_files = glob.glob('tests/**/*.tr', recursive=True) + glob.glob('tests/**/*.turf', recursive=True)
    test_files = [f for f in test_files if f not in exclude_files]
    
    if not test_files:
        print_color("No test files found!", "red")
        sys.exit(1)
        
    print(f"Found {len(test_files)} tests. Running...")
    
    passed = 0
    failed = 0
    failures = []
    
    for f in test_files:
        # Use a unique binary name to prevent "Text file busy"
        bin_path = f'/tmp/turf_test_bin_{os.getpid()}_{hash(f) % 1000000}'
        # Run the turf compiler and execute
        res = subprocess.run(['./turf', f, '-o', bin_path], capture_output=True, text=True)
        
        is_error_test = "error" in f.lower() or "_err" in f.lower() or "undeclared" in f.lower() or "bad_" in f.lower() or "broken_" in f.lower()

        if res.returncode == 0:
            if is_error_test:
                print_color(f"✗ FAIL (Expected Error but Compiled): {f}", "red")
                failed += 1
                failures.append((f, "Test was expected to fail compilation but succeeded."))
            else:
                # Also run the generated binary if it exists
                if os.path.exists(bin_path):
                    try:
                        exec_res = subprocess.run([bin_path], capture_output=True, text=True, timeout=2)
                        if exec_res.returncode == 0:
                            print_color(f"✓ PASS: {f}", "green")
                            passed += 1
                        else:
                            print_color(f"✗ FAIL (Execution): {f}", "red")
                            failed += 1
                            failures.append((f, exec_res.stderr.strip() or "Runtime error or segfault"))
                    except subprocess.TimeoutExpired:
                        print_color(f"✗ FAIL (Timeout): {f}", "red")
                        failed += 1
                        failures.append((f, "Execution timed out (infinite loop?)"))
                    finally:
                        if os.path.exists(bin_path):
                            os.remove(bin_path) # Clean up
                else:
                    print_color(f"✓ PASS (Compile only): {f}", "yellow")
                    passed += 1
        else:
            if is_error_test:
                print_color(f"✓ PASS (Expected Error): {f}", "green")
                passed += 1
            else:
                print_color(f"✗ FAIL (Compile): {f}", "red")
                failed += 1
                failures.append((f, res.stderr.strip() or res.stdout.strip()))
            
    print("\n" + "="*40)
    print("Test Summary:")
    print_color(f"Passed: {passed}", "green" if passed > 0 else "reset")
    print_color(f"Failed: {failed}", "red" if failed > 0 else "reset")
    print(f"Time taken: {time.time() - start_time:.2f}s")
    print("="*40)
    
    if failed > 0:
        print("\nFailure Details:")
        for f, err in failures[:10]: # Show up to 10 failures
            print(f"--- {f} ---")
            lines = err.split('\n')
            for line in lines[:3]:
                print(f"  {line}")
            if len(lines) > 3:
                print("  ...")
        
        if failed > 10:
            print(f"\n... and {failed - 10} more failures.")
            
        sys.exit(1)
        
    sys.exit(0)

if __name__ == '__main__':
    main()

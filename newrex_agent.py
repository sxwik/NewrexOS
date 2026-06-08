import os
import sys
import time
from google import genai

# The Newrex Uplink (Next-Gen SDK)
client = genai.Client(api_key="____redaracted____") 

def preprocess_newrex_code(input_file, output_file):
    print(f"[NWM-AI] Scanning {input_file} for esoteric logic...")
    
    with open(input_file, 'r') as f:
        cursed_code = f.read()

    prompt = f"""
    You are the Newrex bare-metal OS preprocessor. 
    The following C/Assembly code uses an esoteric binary syntax identified by the prefix '0n'.
    When you see '0n' followed by '+' (which means 1) and 'x' (which means 0), you must translate it into standard '0b' binary.
    For example: '0n+x+x' becomes '0b1010'. 
    
    Translate ONLY the esoteric '0n' binary. DO NOT alter any other logic, math, or C/ASM syntax.
    Return ONLY the raw, valid C/Assembly code. No markdown formatting, no explanations.
    
    Code to Process:
    {cursed_code}
    """
    
    # The Auto-Cooldown Loop
    while True:
        try:
            response = client.models.generate_content(
                model='gemini-2.5-flash',
                contents=prompt,
            )
            
            # Strip markdown wrappers
            clean_code = response.text.strip("`c\n").strip("`asm\n").strip("`")
            
            with open(output_file, 'w') as f:
                f.write(clean_code)
                
            print(f"[NWM-AI] Translation complete. Output locked to {output_file}")
            break # Exit the loop if successful
            
        except Exception as e:
            error_message = str(e)
            if "429" in error_message or "Quota" in error_message:
                print("[NWM-AI] Uplink congested (Rate Limit Hit). Cooling down for 35 seconds...")
                time.sleep(35) # Wait for the API to allow requests again
            else:
                print(f"[NWM-AI] Fatal Uplink Error: {error_message}")
                sys.exit(1)

if __name__ == "__main__":
    if len(sys.argv) != 3:
        print("Usage: python3 newrex_agent.py <input.nx> <output.c>")
        sys.exit(1)
    preprocess_newrex_code(sys.argv[1], sys.argv[2])
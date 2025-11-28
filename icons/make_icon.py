#!/usr/bin/env python3
import sys
from PIL import Image

def remove_white_global(im, threshold=245):
    """
    Converts all white-ish pixels to transparent, regardless of where they are.
    This handles 'holes' inside text or objects.
    """
    im = im.convert('RGBA')
    width, height = im.size
    
    # Create a mask where pixels are white-ish
    # We convert to grayscale to find bright areas, but to be precise 
    # we should check all R, G, B channels.
    r, g, b, a = im.split()
    
    # Define a function to threshold: 0 if >= threshold, else 255
    # This will be used for the new alpha channel
    fn = lambda x: 0 if x >= threshold else 255
    
    rmask = r.point(fn, mode='1')
    gmask = g.point(fn, mode='1')
    bmask = b.point(fn, mode='1')
    
    # Combine masks: only transparent if ALL R,G,B are >= threshold
    # In '1' mode, & is logical AND. 
    # But we want: new_alpha = 0 if (r>=T and g>=T and b>=T) else old_alpha
    # So we want to find where (r>=T and g>=T and b>=T) is true.
    
    # Let's use 'L' mode and math to be sure
    r_is_white = r.point(lambda x: 255 if x >= threshold else 0)
    g_is_white = g.point(lambda x: 255 if x >= threshold else 0)
    b_is_white = b.point(lambda x: 255 if x >= threshold else 0)
    
    from PIL import ImageChops
    is_white = ImageChops.lighter(r_is_white, ImageChops.lighter(g_is_white, b_is_white))
    # Wait, 'lighter' is max(a,b). If any is 255, it's 255. That's OR.
    # We want AND. So we use 'darker' which is min(a,b).
    is_white = ImageChops.darker(r_is_white, ImageChops.darker(g_is_white, b_is_white))
    
    # Now is_white is 255 where it's white, 0 otherwise.
    # New alpha: if is_white == 255 then 0 else original_alpha
    new_alpha = ImageChops.subtract(a, is_white)
    
    im.putalpha(new_alpha)
    return im

def main():
    src = sys.argv[1] if len(sys.argv) > 1 else 'ah64.png'
    out = sys.argv[2] if len(sys.argv) > 2 else 'ah64.ico'
    
    try:
        im = Image.open(src)
    except FileNotFoundError:
        print(f"Error: {src} not found.")
        return

    # Process all white areas globally
    im = remove_white_global(im)
    
    # Ensure square for ICO
    max_side = max(im.width, im.height)
    if im.width != im.height:
        new = Image.new('RGBA', (max_side, max_side), (0,0,0,0))
        new.paste(im, ((max_side - im.width)//2, (max_side - im.height)//2), im)
        im = new

    sizes = [(16,16),(32,32),(48,48),(64,64),(128,128),(256,256)]
    # Use 256x256 as base image for multi-res ICO
    base = im.resize((256, 256), Image.LANCZOS)
    base.save(out, format='ICO', sizes=[(s[0], s[1]) for s in sizes])
    
    # Save a PNG copy for verification
    im.save('ah64_transparent_all.png')
    print(f"Done. Saved {out} (Global white-to-transparent) and ah64_transparent_all.png")

if __name__ == '__main__':
    main()

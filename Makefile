EXEC=outguess_image_compare

$(EXEC): main.o
	gcc $< -Ljpeg-6b-steg -l:libjpeg.a -o $@

%.o: %.c
	gcc -c $< -o $@

.PHONY: clean mrproper
clean:
	rm main.o
mrproper: clean
	rm $(EXEC)

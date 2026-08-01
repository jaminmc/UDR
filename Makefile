DIRS = udt src
TARGETS = all clean

$(TARGETS): %: $(patsubst %, %.%, $(DIRS))

$(foreach TGT, $(TARGETS), $(patsubst %, %.$(TGT), $(DIRS))):
	$(MAKE) -C $(subst ., , $@)

# Only the UDR binary is installed (UDT ships as a static lib linked in).
install: all
	$(MAKE) -C src install
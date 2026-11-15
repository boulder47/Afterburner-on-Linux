| 0 | `0x1d6a0` | `0xbd90` | AppleAfterburner::cardStatus(u64 *) | `0x60f` | `0x2a` |
| 1 | `0x1d6b0` | `0x10198` | ?? 0x10198 (data) | `0x30f` | `0x4e` |
| 2 | `0x1d6c0` | `0xbda0` | AppleAfterburner::getPercent(u64 *) | `0x30f` | `0x74` |
| 3 | `0x1d6d0` | `0x821c` | AppleAfterburner::spiprogram(uint32, uint32, void *, u64ll, char) | `0x30f` | `0x9e` |
| 4 | `0x1d6e0` | `0xa67c` | AppleAfterburner::submitData(sContext *, IOMemoryDescriptor *, uint32, u64 *, uint32, uint32, uint32, u64, u64) | `0xb0f` | `0xe9` |
| 5 | `0x1d6f0` | `0x141e0` | ?? 0x141e0 (data) | `0x30f` | `0x10d` |
| 6 | `0x1d700` | `0xa28c` | AppleAfterburner::getSegments(sContext *, IOMemoryDescriptor *, IODMACommand::Segment64 *, uint32 *, u64, u64, u64) | `0x30f` | `0x171` |
| 7 | `0x1d710` | `0x8628` | AppleAfterburner::loadBitfile(uchar const *, uint32) | `0x30f` | `0x19a` |
| 8 | `0x1d720` | `0x6a00` | AppleAfterburner::sleep_timer(IOTimerEventSource *) | `0x30f` | `0x1d4` |
| 9 | `0x1d730` | `0xa9ac` | AppleAfterburner::submitFrame(sContext *, IOMemoryDescriptor *, (subst), u64 *, uint32, uint32, uint32, sDescriptorLengthOffsetInfo) | `0x30f` | `0x23d` |
| 10 | `0x1d740` | `0xbf6c` | AppleAfterburner::unbindFrame(IOMemoryDescriptor *) | `0x30f` | `0x277` |
| 11 | `0x1d750` | `0xca80` | AppleAfterburner::didTerminate(IOService *, uint32, char *) | `0x30f` | `0x2ab` |
| 12 | `0x1d760` | `0xbfec` | AppleAfterburner::checkForSpace(u64 *) | `0x30f` | `0x2d4` |
| 13 | `0x1d770` | `0x7c60` | AppleAfterburner::setPowerState(u64ll, IOService *) | `0x30f` | `0x307` |
| 14 | `0x1d780` | `0xca68` | AppleAfterburner::willTerminate(IOService *, uint32) | `0x30f` | `0x33a` |
| 15 | `0x1d790` | `0x8580` | AppleAfterburner::change_pointer(uint32) | `0x30f` | `0x363` |
| 16 | `0x1d7a0` | `0x74c4` | AppleAfterburner::reconfig_timer(IOTimerEventSource *) | `0x30f` | `0x3a0` |
| 17 | `0x1d7b0` | `0x8af0` | AppleAfterburner::submitResource(IOBufferMemoryDescriptor *, uint32) | `0x30f` | `0x3e4` |
| 18 | `0x1d7c0` | `0x9be0` | AppleAfterburner::updateIdleTime(void) | `0x30f` | `0x40d` |
| 19 | `0x1d7d0` | `0x5aec` | AppleAfterburner::handleInterrupt(OSObject *, IOInterruptEventSource *, int) | `0x30f` | `0x45a` |
| 20 | `0x1d7e0` | `0xb88c` | AppleAfterburner::queryCompletion(sContext *, u64 *, (subst)) | `0x30f` | `0x492` |
| 21 | `0x1d7f0` | `0x803c` | AppleAfterburner::readDMARegister(uint32) | `0x30f` | `0x4bc` |
| 22 | `0x1d800` | `0x985c` | AppleAfterburner::submitDataGated(sDmaConfig *) | `0x30f` | `0x4f2` |
| 23 | `0x1d810` | `0x55c0` | AppleAfterburner::readBAR0Register(uint32) | `0x30f` | `0x51d` |
| 24 | `0x1d820` | `0x6b94` | AppleAfterburner::reconfig_handler(IOInterruptEventSource *, int) | `0x30f` | `0x561` |
| 25 | `0x1d830` | `0xbe64` | AppleAfterburner::updateActiveTime(void) | `0x30f` | `0x58c` |
| 26 | `0x1d840` | `0x8030` | AppleAfterburner::writeDMARegister(uint32, uint32) | `0x30f` | `0x5b8` |
| 27 | `0x1d850` | `0x9c5c` | AppleAfterburner::getDescriptorList(sContext *, IODMACommand::Segment64 *, uint32, uint32, u64 *, uint32 *, u64) | `0x30f` | `0x60f` |
| 28 | `0x1d860` | `0x8084` | AppleAfterburner::readFlashRegister(uint32) | `0x30f` | `0x63b` |
| 29 | `0x1d870` | `0x8024` | AppleAfterburner::writeBAR0Register(uint32, uint32) | `0x30f` | `0x668` |
| 30 | `0x1d880` | `0x5ad4` | AppleAfterburner::readDecodeRegister(uint32) | `0x30f` | `0x695` |
| 31 | `0x1d890` | `0x8090` | AppleAfterburner::writeFlashRegister(uint32, uint32) | `0x30f` | `0x6c3` |
| 32 | `0x1d8a0` | `0x5a3c` | AppleAfterburner::configureInterrupts(void) | `0x30f` | `0x6f1` |
| 33 | `0x1d8b0` | `0x8d5c` | AppleAfterburner::getResourceSegments(IOBufferMemoryDescriptor *, IODMACommand::Segment64 *, uint32 *, u64) | `0x30f` | `0x757` |
| 34 | `0x1d8c0` | `0x806c` | AppleAfterburner::readBitfileRegister(uint32) | `0x30f` | `0x785` |
| 35 | `0x1d8d0` | `0x8054` | AppleAfterburner::readThermalRegister(uint32) | `0x30f` | `0x7b3` |
| 36 | `0x1d8e0` | `0x55cc` | AppleAfterburner::readVersionRegister(uint32) | `0x30f` | `0x7e1` |
| 37 | `0x1d8f0` | `0x9698` | AppleAfterburner::submitResourceGated(sDmaConfig *) | `0x30f` | `0x81b` |
| 38 | `0x1d900` | `0x8014` | AppleAfterburner::writeDecodeRegister(uint32, uint32) | `0x30f` | `0x84a` |
| 39 | `0x1d910` | `0xb634` | AppleAfterburner::queryCompletionGated(u64 *, int *) | `0x30f` | `0x87c` |
| 40 | `0x1d920` | `0x87c8` | AppleAfterburner::resourceLoadCallback(uint32, int, void const *, uint32, void *) | `0x30f` | `0x8b2` |
| 41 | `0x1d930` | `0x8078` | AppleAfterburner::writeBitfileRegister(uint32, uint32) | `0x30f` | `0x8e2` |
| 42 | `0x1d940` | `0x8060` | AppleAfterburner::writeThermalRegister(uint32, uint32) | `0x30f` | `0x912` |
| 43 | `0x1d950` | `0x8048` | AppleAfterburner::writeVersionRegister(uint32, uint32) | `0x30f` | `0x942` |
| 44 | `0x1d960` | `0x7c1c` | AppleAfterburner::pointer_restore_timer(IOTimerEventSource *) | `0x30f` | `0x986` |
| 45 | `0x1d970` | `0x7508` | AppleAfterburner::pointer_restore_handler(IOInterruptEventSource *, int) | `0x30f` | `0x9d1` |
| 46 | `0x1d980` | `0x90d0` | AppleAfterburner::getResourceDescriptorList(IODMACommand::Segment64 *, uint32, uint32, u64 *, uint32 *, u64) | `0x30f` | `0xa26` |
| 47 | `0x1d990` | `0x55d8` | AppleAfterburner::updateIORegistryProperties(void) | `0x30f` | `0xa5b` |
| 48 | `0x1d9a0` | `0x888c` | AppleAfterburner::resourceLoadCallbackHandler(uint32, int, void const *, uint32) | `0x30f` | `0xa96` |
| 49 | `0x1d9b0` | `0xc984` | AppleAfterburner::free(void) | `0x30f` | `0xab4` |
| 50 | `0x1d9c0` | `0xc8c0` | AppleAfterburner::init(OSDictionary *) | `0x30f` | `0xae0` |
| 51 | `0x1d9d0` | `0xc0a4` | AppleAfterburner::stop(IOService *) | `0x30f` | `0xb08` |
| 52 | `0x1d9e0` | `0xc998` | AppleAfterburner::probe(IOService *, int *) | `0x30f` | `0xb33` |
| 53 | `0x1d9f0` | `0xaea8` | AppleAfterburner::reset(void) | `0x30f` | `0xb52` |
| 54 | `0x1da00` | `0x4254` | AppleAfterburner::start(IOService *) | `0x30f` | `0xb7b` |
| 55 | `0x1da10` | `0xb0ec` | AppleAfterburner::cleanUp(sContext *) | `0x30f` | `0xba5` |
| 56 | `0x1da20` | `0xcab0` | AppleAfterburner::finalize(uint32) | `0x30f` | `0xbc7` |
| 57 | `0x1da30` | `0x809c` | AppleAfterburner::spierase(uint32) | `0x30f` | `0xbe9` |
| 58 | `0x1da40` | `0x4000` | MetaClass::ctor() | `0x30f` | `0xc0e` |
| 59 | `0x1da50` | `0x40f8` | MetaClass::ctor() | `0x30f` | `0xc33` |
| 60 | `0x1da60` | `0xbedc` | AppleAfterburner::bindFrame(IOMemoryDescriptor *) | `0x30f` | `0xc6a` |
| 61 | `0x1da70` | `0xc058` | AppleAfterburner::freeSpace(u64 *) | `0xb0f` | `0xc8e` |

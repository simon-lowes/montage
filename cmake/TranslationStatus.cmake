# How far a translation is done, for Window › Language: "<finished> <drafts> <texts>" from translations/montage_*.ts
# (cmake -DTS=<file.ts> -DOUT=<file> -P TranslationStatus.cmake). A draft is a text translated but still unfinished.
file(READ "${TS}" ts)
string(REGEX MATCHALL "<translation[ >/]" all "${ts}")
string(REGEX MATCHALL "<translation type=\"(vanished|obsolete)\"" gone "${ts}")
string(REGEX MATCHALL "<translation type=\"unfinished\">[^<\n]" drafts "${ts}")
string(REGEX MATCHALL "<translation>" finished "${ts}")
list(LENGTH all n_all)
list(LENGTH gone n_gone)
list(LENGTH drafts n_drafts)
list(LENGTH finished n_finished)
math(EXPR n_texts "${n_all} - ${n_gone}")
file(WRITE "${OUT}" "${n_finished} ${n_drafts} ${n_texts}\n")

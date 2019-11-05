#include "marian.h"

#include "common/cli_wrapper.h"
#include "common/logging.h"
#include "data/vocab.h"

int main(int argc, char** argv) {
  using namespace marian;

  createLoggers();

  Ptr<Options> options = New<Options>();
  {
    auto cli = New<cli::CLIWrapper>(
        options,
        "Create a vocabulary from text corpora given on STDIN",
        "Allowed options",
        "Examples:\n"
        "  ./marian-vocab < text.src > vocab.yml\n"
        "  cat text.src text.trg | ./marian-vocab > vocab.yml");
    cli->add<size_t>("--max-size,-m", "Generate only UINT most common vocabulary items", 0);
    cli->parse(argc, argv);
    options->rebuild(); // Required when using CLIWrapper as it only modifies the underlying YAML object,
                        // the Options object is unaware of any changes and does not trigger a rebuild.
                        // This is not a problem when ConfigParser is used. Only needed for custom options like here.
                        // @TODO: make CLIWrapper remember the Options object.
  }

  LOG(info, "Creating vocabulary...");

  auto vocab = New<Vocab>(options, 0);
  vocab->create("stdout", "stdin", options->get<size_t>("max-size"));

  LOG(info, "Finished");

  return 0;
}

#include "dsl/statement.hpp"
#include "dsl/type_description.hpp"
#include "dsl/entity.hpp"

namespace {
  std::string R_LS   = "^\\s*";                   // line start
  std::string R_LE   = "(//.*)?\\s*$";        // line end
  std::string R_SE   = ";\\s*"+R_LE;              // statement end
  std::string R_VAR  = "\\s*([a-zA-Z0-9_]+)\\s*"; // variable (or a const number)
  std::string R_ARGL = "\\s*[(](.*)[)]\\s*";      // arguement list, need a 2nd level parsing
  std::string R_R    = R_VAR+":("+R_VAR;          // range
  std::string R_SI   = "(\\["+R_VAR+"])?\\s*";   // single index
  std::string R_RI   = "\\s*\\["+R_R+"]\\s*";     // range index

  void parse_arglist(const char *plist, std::list<std::string> & params) {
    static char param[2048];
    strcpy(param, plist);
    char *p = strtok(param, " ,");
    while(p != NULL) { if(strlen(p)) params.push_back(std::string(p)); p = strtok(NULL, " ,"); }
  }
}

CodeGen::CodeGen() {
  typedb.init();

  decoders.push_back(new StatementBlank);
  decoders.push_back(new StatementComment);
  decoders.push_back(new StatementNameSpace);
  decoders.push_back(new StatementConst);
  decoders.push_back(new StatementTypeDef);
  decoders.push_back(new StatementCreate);
  decoders.push_back(new StatementConnect);

  decoders.push_back(new StatementError); // always the final one

}

CodeGen::~CodeGen() {
  for(auto d:decoders) delete d;
}

bool CodeGen::parse_int(const std::string &param, int &rv) {
  if(consts.count(param)) {
    rv = consts[param];
    return true;
  }

  try { rv = std::stoi(param); }
  catch(std::invalid_argument &e) {
    std::cerr << "[Integer] Fail to parse `" << param << "' into an integer." << std::endl;
    return false;
  }

  return true;
}

bool CodeGen::parse_bool(const std::string &param, bool &rv) {
  if(param == "true"  || param == "TRUE" || param == "T") { rv = true; return true; }
  if(param == "false" || param == "FALSE" || param == "F") { rv = false; return true; }
  
  if(consts.count(param)) {
    rv = consts[param];
    return true;
  }

  try { rv = std::stoi(param); }
  catch(std::invalid_argument &e) {
    std::cerr << "[Integer] Fail to parse `" << param << "' into boolean." << std::endl;
    return false;
  }

  return true;
}

void CodeGen::emit_hpp(std::ofstream &file) {
  file << "#include <vector>" << std::endl;
  for(auto h:header_list) file << "#include \"" << h << "\"" << std::endl;
  file << std::endl;
  if(!space.empty()) file << "namespace " << space << " {\n" << std::endl;
  for(auto def:type_declarations) def->emit(file);
  for(auto e:entities) e->emit_declaration(file, true);
  if(!space.empty()) file << "\n}" << std::endl;
}

void CodeGen::emit_cpp(std::ofstream &file, const std::string& h) {
  file << "#include \"" << h << "\"" << std::endl;
  if(!space.empty()) file << "namespace " << space << " {\n" << std::endl;
  for(auto e:entities) e->emit_declaration(file, false);
  file << "void init() {" << std::endl;
  for(auto e:entities) e->emit_initialization(file);
  file << "}" << std::endl;
  if(!space.empty()) file << "\n}" << std::endl;
}

CodeGen codegendb;

bool StatementBase::match(const char* line) {
  if(!std::regex_match(line, cm, expression)) return false;
  std::cout << line << std::endl;
  int i=0; for(auto m: cm) std::cout << "cm[" << i++ << "]: " << m << std::endl;
  std::cout << std::endl;
  return true;
}

StatementBlank::StatementBlank() : StatementBase(R_LS+R_LE) {}

bool StatementBlank::decode(const char* line) {
  return match(line); // doing nothing
}

StatementComment::StatementComment() : StatementBase(R_LS+R_SE) {}

bool StatementComment::decode(const char* line) {
  return match(line); // doing nothing
}

StatementTypeDef::StatementTypeDef() : StatementBase(R_LS+"type"+R_VAR+"="+R_VAR+R_ARGL+R_SE) {}

bool StatementTypeDef::decode(const char* line) {
  if(!match(line)) return false;
  std::list<std::string> params;
  parse_arglist(std::string(cm[3]).c_str(), params);
  if(typedb.create(cm[1], cm[2], params)) return true;

  // report as failed to decode
  std::cerr << "[Decode] Cannot decode: type " << cm[1] << " = " << cm[2] << "(";
  int i=1; for(auto p:params) std::cerr << p << (params.size() == i++ ? ")" : ",");
  std::cerr << std::endl;
  return false;
}

StatementConst::StatementConst() : StatementBase(R_LS+"const"+R_VAR+"="+R_VAR+R_SE) {}

bool StatementConst::decode(const char* line) {
  if(!match(line)) return false;

  std::string name(cm[1]);
  if(codegendb.consts.count(name)) {
    std::cerr << "[Double Definition] Const `" << name << "' has already been defined!" << std::endl;
    return false;
  }

  if(!codegendb.parse_int(cm[2], codegendb.consts[cm[1]])) return false;
  return true;
}

StatementCreate::StatementCreate() : StatementBase(R_LS+"create"+R_VAR+"="+R_VAR+R_SI+R_SE) {}

bool StatementCreate::decode(const char* line) {
  if(!match(line)) return false;

  std::string name(cm[1]);
  std::string type_name(cm[2]);
  int size = 1; // default
  if(cm[4].length() && !codegendb.parse_int(cm[4], size)) return false;
  entitydb.create(name, type_name, size);
  return true;
}

StatementNameSpace::StatementNameSpace() : StatementBase(R_LS+"namespace"+R_VAR+R_SE) {}

bool StatementNameSpace::decode(const char* line) {
  if(!match(line)) return false;
  codegendb.space = cm[1];
  return true;
}

StatementConnect::StatementConnect() : StatementBase(R_LS+"connect"+R_VAR+R_SI+"->"+R_VAR+R_SI+R_SE) {}

bool StatementConnect::decode(const char* line) {
  if(!match(line)) return false;
  return true;
}

StatementError::StatementError() :  StatementBase("") {}

bool StatementError::decode(const char* line) {
  std::cerr << "[Decode] cannot parse line: " << std::endl;
  std::cerr << std::string(line) << std::endl;
  exit(-1);
}

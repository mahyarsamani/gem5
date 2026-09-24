from slicc.ast.DeclAST import DeclAST

class CheckParametersAST(DeclAST):
    def __init__(self, slicc, statements):
        super().__init__(slicc)
        self.statements = statements

    def __repr__(self):
        return "[CheckParametersAST]"

    def generate(self):
        machine = self.symtab.state_machine
        if machine:
            code = self.slicc.codeFormatter()
            self.statements.generate(code, None)
            machine.addCheckParameters(str(code))
